#!/usr/bin/env python3
"""Validate voyant SIL MF4 traces: channels present, units correct, values sane.

A round-trip check on the per-test `.mf4` files the harness drops when
`PCS_SIL_TRACE_DIR` is set (`PCS_SIL_TRACE_DIR=build/traces cargo test`, or via
`tools/run_sil.sh` with the env exported). Each Sil-world test dumps `<test-fn>.mf4`;
this validates the two flagship traces — `end_to_end.mf4` (the `end_to_end` test) and
`adc_ports.mf4` (the `adc_ports` test). Opens each with asammdf and asserts named
channels exist with the right unit and sample counts, a known value lands where the
model says it should, an enum channel renders text, and the firmware clock starts at
~one tick and increments 1000 us/tick (value-only — no sim-axis alignment).

Usage:
    python tools/validate_mf4.py [--dir build/traces]

Exits nonzero if any assertion fails.
"""

import argparse
import os
import sys

from asammdf import MDF

FAILS = []


def check(desc, ok, detail=""):
    tag = "PASS" if ok else "FAIL"
    print(f"  [{tag}] {desc}" + (f"  ({detail})" if detail else ""))
    if not ok:
        FAILS.append(desc)


def channel(mdf, name):
    """Fetch a channel by full name, or None if absent."""
    try:
        return mdf.get(name)
    except Exception:
        return None


def check_unit(mdf, name, expected_unit, min_samples=1):
    sig = channel(mdf, name)
    if sig is None:
        check(f"{name} present", False)
        return None
    n = len(sig.samples)
    check(f"{name} present with {n} sample(s)", n >= min_samples, f"n={n}")
    check(
        f"{name} unit == {expected_unit!r}",
        sig.unit == expected_unit,
        f"got {sig.unit!r}",
    )
    return sig


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", default="build/traces", help="directory holding the .mf4 traces")
    args = ap.parse_args()

    e2e_path = os.path.join(args.dir, "end_to_end.mf4")
    adc_path = os.path.join(args.dir, "adc_ports.mf4")

    print(f"== {e2e_path} ==")
    m = MDF(e2e_path)

    # ZOH materialization: even a once-written signal spans the run (change sample
    # + terminal sample at run end), so every channel is scrubbable in the GUI.
    angle = check_unit(m, "vsig:as5048_motor:angle", "rad", min_samples=2)
    run_end = 0.0
    if angle is not None:
        last = float(angle.samples[-1])
        check(
            "angle ends at ~pi/2 (90 deg commanded -> 1.5708 rad)",
            abs(last - 1.5707963) < 1e-3,
            f"last={last:.6f}",
        )
        run_end = float(angle.timestamps[-1])
        check(
            "angle extends to the run end (ZOH terminal sample)",
            run_end > float(angle.timestamps[0]),
            f"span {angle.timestamps[0]:.6f}..{run_end:.6f} s",
        )

    raw = check_unit(m, "vsig:as5048_motor:raw_encoder_ticks", "counts")
    if raw is not None:
        last = int(raw.samples[-1])
        check("raw_encoder_ticks ends at 4096 (16384/4)", last == 4096, f"last={last}")

    # PWM_U_duty: registered unitless -> asammdf must show an EMPTY unit, not a default.
    duty = check_unit(m, "vsig:pcs_bldc:PWM_U_duty", "", min_samples=2)
    if duty is not None and run_end > 0.0:
        check(
            "PWM_U_duty (constant 0) spans to the same run end",
            abs(float(duty.timestamps[-1]) - run_end) < 1e-9,
            f"last t={float(duty.timestamps[-1]):.6f} s",
        )

    # A firmware cvar is present with samples (cvars carry no unit).
    cvar = channel(m, "cvar:pcs_bldc:task1msRuns")
    check(
        "cvar:pcs_bldc:task1msRuns present with samples",
        cvar is not None and len(cvar.samples) > 0,
        f"n={0 if cvar is None else len(cvar.samples)}",
    )

    # Firmware clock (value-only, no sim-axis alignment): from reset the first recorded
    # sample is ~one tick and every step adds exactly 1000 us.
    clk = channel(m, "cvar:pcs_bldc:lib_timer_data.currentTime_us")
    if clk is None:
        check("cvar:pcs_bldc:lib_timer_data.currentTime_us present", False)
    else:
        s = [int(x) for x in clk.samples]
        check(
            "firmware clock's first sample is ~one tick from reset (<= a few ticks)",
            bool(s) and 1000 <= s[0] <= 5000,
            f"first={s[0] if s else None}",
        )
        # Every real per-tick step is 1000 us; the only 0 diff is the ZOH terminal
        # sample (a materialization artifact repeating the last value at run end).
        nonzero = {b - a for a, b in zip(s, s[1:]) if b != a}
        check(
            "firmware clock increments 1000 us/tick",
            nonzero == {1000},
            f"nonzero step diffs seen: {sorted(nonzero)}",
        )

    # An enum channel renders as TEXT via its value-to-text conversion (a numeric
    # channel would come back numeric). The enumerator *names* in this firmware build
    # are DWARF placeholders (`<N>`); the pipeline carries whatever the historian
    # stored, so this asserts the mechanism (text out, not a bare number).
    enum_name = "cvar:pcs_bldc:HW_ADC_channelConfig[0].triggerMode"
    en = channel(m, enum_name)
    if en is None:
        check(f"{enum_name} present", False)
    else:
        samples = en.physical().samples  # applies the conversion
        first = samples[0]
        is_text = isinstance(first, (bytes, bytearray, str))
        rendered = first.decode() if isinstance(first, (bytes, bytearray)) else str(first)
        check(f"{enum_name} renders as text via value-to-text", is_text, f"first={rendered!r}")

    print(f"\n== {adc_path} ==")
    ma = MDF(adc_path)
    adc = check_unit(ma, "vsig:pcs_bldc:ADC1_IN6", "V")
    if adc is not None:
        check("ADC1_IN6 has a nonzero driven sample", any(float(s) != 0.0 for s in adc.samples))

    print()
    if FAILS:
        print(f"VALIDATION FAILED: {len(FAILS)} assertion(s)")
        return 1
    print("VALIDATION PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
