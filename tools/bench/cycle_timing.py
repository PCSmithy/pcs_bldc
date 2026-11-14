#!/usr/bin/env python3
"""Bench check of the PWM-cycle durations, read from the running board over SWD.

Reads the duration probes without halting the core, for a board that is
driving the motor, and checks them against the timing requirement. Writes a
dated record to docs/bench/cycle-timing.md.

Run:  .venv/Scripts/python tools/bench/cycle_timing.py [--seconds 60]

The operator sets up the acceptance condition first (sys~mc_006): the maximum
speed target, four 4-byte one-cycle watches and 28 4-byte watches at 10 ms.
The script reads the drive state, the speed and the active watch count and
fails the run if they do not match.

Tools: OpenOCD and arm-none-eabi-gdb, found in an STM32CubeIDE install or
named with --openocd / --openocd-scripts / --gdb.
"""

import argparse
import datetime
import glob
import math
import re
import struct
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
ELF = REPO / "build" / "arm-fw" / "src" / "pcs_bldc_fw.elf"
RECORD = REPO / "docs" / "bench" / "cycle-timing.md"

PWM_PERIOD_US = 50.0
STEP_BOUND_US = 20      # sys~mc_006
CALLBACK_BOUND_US = 40  # sys~mc_006
WATCHES_REQUIRED = 32   # sys~mc_006: four one-cycle + 28 at 10 ms
ICSR = 0xE000ED04       # SCB interrupt control and state: VECTACTIVE in bits 8:0
ADC_EXCEPTION = 16 + 18  # ADC1_2_IRQn
DRIVING = 3             # Motor::DriveState::Driving

MOTOR = "app_motorControl_data.channels._M_elems[0]"
SYMBOLS = {
    "step": "main_cycleProbe_step_us",
    "callback": "main_cycleProbe_callback_us",
    "stepMax": "main_cycleProbe_stepMax_us",
    "callbackMax": "main_cycleProbe_callbackMax_us",
    "irq": "HW_ADC_data.irqDuration_us",
    "irqMax": "HW_ADC_data.irqDurationMax_us",
    "irqTime": "HW_ADC_data.irqTime_us",
    "driveState": f"{MOTOR}.driveState",
    "velocity": f"{MOTOR}.velocityMeasured_radPerSec",
    "fault": f"{MOTOR}.faultLatched",
    "maxVelocity": "app_motorControl_channelConfig[0].maxVelocity_radPerSec",
    "watches": "app_server_trace_data.activeCount",
    "identity": "lib_build_identityString",
}


def find_tool(pattern, what):
    hits = sorted(glob.glob(pattern))
    if not hits:
        sys.exit(f"{what} not found ({pattern}); name it on the command line")
    return hits[-1]


def resolve(gdb, elf):
    """Symbol addresses and the image's build identity, from the ELF's DWARF."""
    cmd = [gdb, "-batch", str(elf)]
    for expr in SYMBOLS.values():
        cmd += ["-ex", f"print/x (unsigned)&({expr})"]
    cmd += ["-ex", "print lib_build_identityString"]
    out = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout
    values = re.findall(r"^\$\d+ = (.*)$", out, re.M)
    if len(values) != len(SYMBOLS) + 1:
        sys.exit(f"gdb resolved {len(values)} of {len(SYMBOLS) + 1} expressions:\n{out}")
    addrs = {name: int(v, 16) for name, v in zip(SYMBOLS, values)}
    return addrs, re.search(r'"(.*)"', values[-1]).group(1)


def tcl_script(a, seconds):
    """One OpenOCD session: identity, clear the maxima, sample, read the results."""
    rd = lambda addr, n=1: f"[read_memory {addr:#x} 32 {n}]"
    return f"""
init
echo "IDENT [read_memory {a['identity']:#x} 8 32]"
echo "CFG {rd(a['maxVelocity'])}"
mww {a['stepMax']:#x} 0
mww {a['callbackMax']:#x} 0
mww {a['irqMax']:#x} 0
set t0 [clock milliseconds]
echo "START $t0 {rd(a['irqTime'])} {rd(a['irqMax'])}"
set n 0; set inIrq 0; set notDriving 0; set seenIrq 0; set seenStep 0; set seenCb 0
while {{[clock milliseconds] - $t0 < {int(seconds * 1000)}}} {{
    if {{([read_memory {ICSR:#x} 32 1] & 0x1ff) == {ADC_EXCEPTION}}} {{ incr inIrq }}
    incr n
    if {{$n % 50 == 0}} {{
        set v {rd(a['irq'])}; if {{$v > $seenIrq}} {{ set seenIrq $v }}
        set v {rd(a['step'])}; if {{$v > $seenStep}} {{ set seenStep $v }}
        set v {rd(a['callback'])}; if {{$v > $seenCb}} {{ set seenCb $v }}
        if {{([read_memory {a['driveState']:#x} 8 1] != {DRIVING})}} {{ incr notDriving }}
    }}
    if {{$n % 2000 == 0}} {{ echo "V {rd(a['velocity'])}" }}
}}
echo "END [clock milliseconds] {rd(a['irqTime'])} $n $inIrq $notDriving $seenIrq $seenStep $seenCb"
echo "MAX {rd(a['stepMax'])} {rd(a['callbackMax'])} {rd(a['irqMax'])}"
echo "STATE [read_memory {a['fault']:#x} 8 1] {rd(a['watches'])}"
shutdown
"""


def to_float(word):
    return struct.unpack("<f", struct.pack("<I", int(word, 0)))[0]


def main():
    cube = "C:/ST/STM32CubeIDE_*/STM32CubeIDE/plugins/"
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seconds", type=float, default=60.0)
    ap.add_argument("--elf", type=Path, default=ELF)
    ap.add_argument("--openocd")
    ap.add_argument("--openocd-scripts")
    ap.add_argument("--gdb")
    ap.add_argument("--no-record", action="store_true", help="print only; leave the record file alone")
    args = ap.parse_args()

    openocd = args.openocd or find_tool(cube + "*openocd.win32*/tools/bin/openocd.exe", "OpenOCD")
    scripts = args.openocd_scripts or find_tool(cube + "*mcu.debug.openocd_*/resources/openocd/st_scripts", "OpenOCD scripts")
    gdb = args.gdb or find_tool(cube + "*gnu-tools-for-stm32*/tools/bin/arm-none-eabi-gdb.exe", "arm-none-eabi-gdb")

    addrs, image_identity = resolve(gdb, args.elf.resolve())
    cmd = [openocd, "-s", scripts, "-f", "interface/stlink-dap.cfg", "-f", "target/stm32g4x.cfg",
           "-c", tcl_script(addrs, args.seconds)]
    print(f"sampling for {args.seconds:.0f} s ...", flush=True)
    out = subprocess.run(cmd, capture_output=True, text=True, timeout=args.seconds + 60)
    lines = {}
    velocities = []
    for line in (out.stdout + out.stderr).splitlines():
        parts = line.split()
        if parts and parts[0] == "V":
            velocities.append(to_float(parts[1]))
        elif parts and parts[0] in ("IDENT", "CFG", "START", "END", "MAX", "STATE"):
            lines[parts[0]] = parts[1:]
    missing = {"IDENT", "CFG", "START", "END", "MAX", "STATE"} - set(lines)
    if missing:
        sys.exit(f"no {sorted(missing)} from OpenOCD:\n{out.stdout[-2000:]}\n{out.stderr[-2000:]}")

    board_identity = bytes(int(b, 0) for b in lines["IDENT"]).split(b"\0")[0].decode("ascii", "replace")
    if board_identity != image_identity:
        sys.exit(f"the board runs '{board_identity}', the ELF is '{image_identity}': flash this build or pass --elf")

    max_velocity = to_float(lines["CFG"][0])
    t0, irq_time0, _ = (int(x, 0) for x in lines["START"])
    t1, irq_time1, n, in_irq, not_driving, seen_irq, seen_step, seen_cb = (int(x, 0) for x in lines["END"])
    step_max, callback_max, irq_max = (int(x, 0) for x in lines["MAX"])
    fault, watches = (int(x, 0) for x in lines["STATE"])
    elapsed_us = (t1 - t0) * 1000.0
    speed = sum(abs(v) for v in velocities) / max(len(velocities), 1)

    # The handler's share of wall time two ways: its own running total, and the
    # fraction of samples that found the core inside the ADC exception.
    share_probe = ((irq_time1 - irq_time0) % 2**32) / elapsed_us
    share_core = in_irq / n
    sigma = math.sqrt(share_core * (1.0 - share_core) / n)
    mean_probe_us = share_probe * PWM_PERIOD_US
    mean_core_us = share_core * PWM_PERIOD_US
    tolerance_us = 1.0 + 3.0 * sigma * PWM_PERIOD_US

    checks = [
        ("drive in the Driving state at every look, no fault latched", (not_driving == 0) and (fault == 0),
         f"{not_driving} looks not driving, fault {fault}"),
        (f"speed at the {max_velocity:.1f} rad/s maximum target (within 5 %)", speed >= 0.95 * max_velocity,
         f"mean |velocity| {speed:.2f} rad/s"),
        (f"{WATCHES_REQUIRED} trace watches active", watches == WATCHES_REQUIRED, f"{watches} active"),
        # sys~mc_006's bounds: its coverage tag goes on these two once the record passes.
        (f"commutation step maximum within {STEP_BOUND_US} us", step_max <= STEP_BOUND_US, f"{step_max} us"),
        (f"callback maximum within {CALLBACK_BOUND_US} us", callback_max <= CALLBACK_BOUND_US, f"{callback_max} us"),
        # [test->fw~hal_adc_011~1] a cleared maximum restarts, and holds the largest run seen since
        ("interrupt maximum restarted at the clear and bounds every latest duration seen",
         (irq_max >= seen_irq) and (step_max >= seen_step) and (callback_max >= seen_cb),
         f"max {irq_max} us, largest latest seen {seen_irq} us"),
        # [test->fw~hal_adc_011~1] the running total against the core's own exception state
        (f"interrupt total agrees with the time the core spends in the handler (within {tolerance_us:.2f} us per run)",
         abs(mean_probe_us - mean_core_us) <= tolerance_us,
         f"total {mean_probe_us:.2f} us per run, core {mean_core_us:.2f} us per run over {n} samples"),
    ]
    passed = all(ok for _, ok, _ in checks)
    for name, ok, detail in checks:
        print(f"  [{'ok' if ok else 'FAIL'}] {name}: {detail}")
    print("PASS" if passed else "FAIL")

    if not args.no_record:
        RECORD.parent.mkdir(parents=True, exist_ok=True)
        rows = "\n".join(f"| {name} | {detail} | {'pass' if ok else '**fail**'} |" for name, ok, detail in checks)
        RECORD.write_text(
            "# Bench record: PWM-cycle durations\n\n"
            "Written by `tools/bench/cycle_timing.py`; each run replaces it.\n\n"
            f"- Date: {datetime.date.today().isoformat()}\n"
            f"- Firmware identity: `{board_identity}`\n"
            f"- Window: {elapsed_us / 1e6:.1f} s, {n} core samples\n"
            f"- Result: **{'pass' if passed else 'fail'}**\n\n"
            "| Check | Measured | Result |\n|---|---|---|\n" + rows + "\n",
            encoding="utf-8", newline="\n",
        )
        print(f"record: {RECORD.relative_to(REPO)}")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
