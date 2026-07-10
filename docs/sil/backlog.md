# SIL backlog

Deferred cleanup tasks — parked here so they aren't lost, with enough scope
detail to pick up cold. Not roadmap items (see `roadmap.md` for those).

## Remove the `HW_<Module>_sim.h` inject/inspect API layer

**When:** after the firmware/SIL build unification lands and the SIL framework
is feature-complete enough to replace it (State Table + Route Table + sim
step loop, white-box injection working end-to-end).

**What it is:** during initial driver bring-up (pre-SIL), the sim-side hw
drivers grew `HW_<Module>_sim.h` headers exposing `HW_<Module>_sim_*`
inject/inspect functions (set pin state, inject SPI RX, read USB TX, stall
ADC, ...). Files, as of 2026-07-04:

- `sw/lib/c/shared/hw/ADC/sim/HW_ADC_sim.h` (plus `_sim_` decls that leaked
  into `sw/lib/c/shared/hw/ADC/sim/HW_ADC.h`)
- `sw/lib/c/shared/hw/DMA/sim/HW_DMA_sim.h`
- `sw/lib/c/shared/hw/GPIO/sim/HW_GPIO_sim.h`
- `sw/lib/c/shared/hw/OPAMP/sim/HW_OPAMP_sim.h`
- `sw/lib/c/shared/hw/SPI/sim/HW_SPI_sim.h`
- `sw/lib/c/shared/hw/TIM/sim/HW_TIM_sim.h`
- `sw/lib/c/shared/hw/USB/sim/HW_USB_sim.h`

**Why it goes:** the SIL (voyant) has white-box DWARF read/write access to all
firmware memory, plus State Table overrides and Route Table suspend/resume for
injection. A hand-written per-driver injection API is a redundant seam — extra
firmware added to the firmware-under-test in order to test the
firmware-under-test. Remove it across the board, on every peripheral driver.

**Prerequisite met (2026-07-05):** the **port registration seam** now exists
(`SIL_ports` + `sil_fw_setHooks` + voyant's cache-mediated port state; see
`state-route-tables.md` §1 "Ports"), giving drivers a sanctioned, native-format
input/output path — sim `HW_ADC` is the first conversion. Removal itself is
still parked; drivers migrate to ports as they are converted.

**Policy, effective immediately:** do NOT add new consumers of the `_sim_*`
APIs (in C, Rust, or scripts). SIL-side injection/inspection goes through the
white-box path instead — DWARF read/write of the sim drivers' statics (and,
as they land, State Table overrides / Route Table suspend-resume). The
existing 8 test suites are the frozen consumer set this task removes.

**The complication — unit tests:** 8 Unity suites currently exercise drivers
through these `_sim_*` APIs (~108 call sites, audited 2026-07-04):
`hw/{ADC,DMA,GPIO,OPAMP,SPI,TIM,USB}/sim/test/` and
`io/serial/test/test_IO_serial.c`. That usage is also wrong: unit tests should
own sufficient stubs/mocks to fully exercise the module under test, not lean
on test-support code compiled into the shipped driver. For each suite, either
rewrite against proper test-owned stubs/mocks, or retire it in favor of more
comprehensive cross-module SIL tests. No other consumers exist (nothing in
`sw/sil/` or `sw/fw/` uses `_sim_*` except a mention in
`sw/fw/src/hw/sim/_placeholder.c`).

## Future feature: native debugger (VSCode) attached to in-the-loop firmware

**Idea (owner, 2026-07-09):** run voyant with firmware in the loop, connected
to the VSCode debugger — breakpoints, stepping, variable watches in the C
firmware source while the sim runs. Realtime-sim flavored: fine to build the
firmware at `-O0 -g` (the existing `run_sil.sh --debug` flavor), not chasing
100×+ here.

**Feasibility sketch:** most of this comes free from the architecture. The
firmware is a native DLL inside the voyant process, so attaching a native
debugger (MinGW gdb / VSCode `cppdbg`) to that process gives source-level
breakpoints/stepping/watches in the C today — the DLL carries full DWARF, and
gdb shows the executing fiber's stack at a breakpoint. The killer property is
that the sim is single-threaded: **hitting a breakpoint freezes the entire
virtual world coherently** — motor model, sim time, everything — unlike real
hardware, where the plant keeps moving while the CPU is halted; and
determinism means you can replay to the same breakpoint identically.

**To productize (rough):** a `--wait-debugger` / hold mode in the driver +
VSCode `launch.json` attach configs; a pacing policy that tolerates wall-clock
stalls (a breakpoint stalls the paced loop — resync rather than sprint on
resume); docs. Natural host is the **realtime/paced run mode**, so this
sequences after fast mode + Python bindings (D3) and the realtime dashboard
(D4), per the owner's ordering.
