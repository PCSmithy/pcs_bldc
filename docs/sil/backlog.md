# SIL backlog

Deferred cleanup tasks — parked here so they aren't lost, with enough scope
detail to pick up cold. Not roadmap items (see `roadmap.md` for those).

## `usb_cdc` / `teleplot` sig_type — telemetry captured as table signals

**When:** near-term — first sprint after commutation SIL, or opportunistically
during it. Filed 2026-07-12; owner wants it near the top of this backlog.

**What:** capture the sim USB CDC TX stream via upcall into a comms entry
(`usb_cdc` sig_type), and parse the Teleplot text into per-signal table
entries (either a `:decoded` modifier or a dedicated `teleplot` sig_type), so
a test asserts `sim["usb:pcs_bldc:motor_angle"] == 90.0` instead of
DWARF-reading `HW_USB_sim_data.tx[]` byte-by-byte (what `read_tx_capture` in
`pcs_bldc_sil/src/main.rs` does today). Design anchor:
`state-route-tables.md` §1 "Comms entries" (logical payloads via sim-HW
upcall). The full comms design wants D8 one-shot delivery for rx *timing*,
but TX capture + parse needs no D8.

## DuplexTransfer — residual extensions

**What:** the synchronous member↔member serial-transaction primitive landed for
`spi` (stage 2 of the commutation sprint). Remaining:

- **Extend to UART / USB_CDC** when real responder models exist (an IMU, etc.) —
  the same `DuplexPeer` upcall, a new sim-HW bus driver registering the endpoint.
- **D8 one-shot completion timing** for non-blocking (DMA/IT) buses: the data
  plane is DuplexTransfer, but completion *timing* still quantizes to a D8
  one-shot. Blocking SW transfers (the AS5048 path) consume the response
  synchronously and need none of this.
- **Sub-tick event timestamps:** tick-resolution timestamps make same-tick
  transactions share a timestamp; the historian wants finer stamps for
  same-tick ordering.
- **Firmware↔firmware duplex** (two boards on one bus — the multi-device stretch):
  the synchronous primitive needs a Rust-side responder, so one firmware instance
  cannot answer another synchronously. This rides the **D8 delayed-response**
  extension below (the peer callback loads the response and schedules its delivery),
  not the MVP synchronous path.

**Delayed duplex responses via the D8 interrupt table (owner, 2026-07-16).** The
firmware initiates a transfer; the peer callback parses TX, runs arbitrary code,
loads the response buffer, *and configures the response interrupt* (delay +
handler) — letting the framework deliver the response at a future sim time with
roughly-modeled transfer delays. NOT MVP: pursue only when bringing up the D8
interrupt tables, and only if precision-delayed duplex transfers buy real
timing-accuracy. Aligns with `sim-interrupts.md` §3's example (sim `HW_SPI`
scheduling a one-shot `SPI3_IRQHandler` 2 µs out); the new element is the *peer*
configuring the delay/response rather than the driver hardcoding a literal.
Natural fit: non-blocking (DMA/IT) buses where real hardware raises a completion
IRQ after bytes × bit-time — blocking SW transfers gain nothing.

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
- `sw/lib/c/shared/hw/SPI/sim/HW_SPI_sim.h` (SPI's `setInjectedRx` is already
  gone — DuplexTransfer replaced it and the Unity suite was rewritten against a
  test-owned hooks double; the remaining `_sim_*` getters here still apply)
- `sw/lib/c/shared/hw/TIM/sim/HW_TIM_sim.h`
- `sw/lib/c/shared/hw/USB/sim/HW_USB_sim.h`

**Why it goes:** the SIL (voyant) has white-box DWARF read/write access to all
firmware memory, plus Route Table suspend/resume + direct destination writes for
injection. A hand-written per-driver injection API is a redundant seam — extra
firmware added to the firmware-under-test in order to test the
firmware-under-test. Remove it across the board, on every peripheral driver.

**Prerequisite met (2026-07-05):** the **port registration seam** now exists
(`SIL_ports` + `sil_fw_setHooks` + voyant's cache-mediated port state; see
`state-route-tables.md` §1 "Ports"), giving drivers a sanctioned, native-format
input/output path — sim `HW_ADC` is the first conversion. Removal itself is
still parked; drivers migrate to ports as they are converted.

**Removal sequence (pinned 2026-07-17; owner: nothing survives indefinitely).**
A driver's box is checked ONLY when its `HW_<Module>_sim.h` is deleted. Each
conversion = replacement seam on the production path + Unity suite rewritten
against test-owned doubles (the SPI injectedRx pattern) + the header deleted:

- ◐ **SPI** — `setInjectedRx` + loopback replaced by DuplexTransfer (sprint
  stage 2, suite rewritten against a test-owned hooks double), but
  `HW_SPI_sim.h` still exists: `getLastTx` / CS inspection / tick remain in
  Unity use. Final sweep: assert TX via the hooks double's own capture, find
  the CS-observation replacement, then delete the header.
- ☑ **TIM** — sprint stage 4 (PWM/bridge observation ports): duty/enable/MOE
  output ports replace the `_sim_` waveform inspection; break injection is a
  table write to the DWARF-visible MOE static. `HW_TIM_sim.h` and the whole
  carrier/waveform machinery (`HW_TIM_sim_advance`, output/complementary level
  queries, dead-time + trigger-count getters, `assertBreak`, and the counter
  ramp / centerGoingUp / ocConfigured / triggerCount state that only fed them)
  are deleted; `HW_TIM_getCounter` (TIM2 timebase) and `HW_TIM_clearBreakFlags`
  stay (runtime consumers). The Unity suite is rewritten against a test-owned
  `SIL_ports_hooks_S` double (registration + duty/enable/MOE publication); the
  waveform/complementary/dead-time/TRGO/`assertBreak`/counter-direction tests
  are retired (the stage-7 closed loop is their replacement coverage).
- ☐ **GPIO** — sprint stage 6 (button gestures): drive the DWARF-visible input
  statics via `st.write` (policy already forbids `setInputLevel`); decide the
  EXTI-trigger seam; delete `HW_GPIO_sim.h`.
- ☐ **USB** (+ `io/serial` test usage) — with the `usb_cdc`/`teleplot`
  sig_type item above: comms-entry TX capture replaces the capture getters;
  delete `HW_USB_sim.h`.
- ☐ **ADC** (conversion-stall), **DMA**, **OPAMP** — final sweep after sprint
  stage 7: pick per-capability replacements (test-owned hooks double or DWARF
  write), rewrite/retire the suites, delete the headers.
- ☐ **Exit criterion / enforcement:** no `*_sim.h` files remain under
  `sw/lib/c/shared/hw/`, and a grep for `_sim_` there comes back empty —
  worth a CI lint line once the last header falls, so the crutch can't grow
  back.

**Policy, effective immediately:** do NOT add new consumers of the `_sim_*`
APIs (in C, Rust, or scripts). SIL-side injection/inspection goes through the
white-box path instead — DWARF read/write of the sim drivers' statics (and,
as they land, Route Table suspend-resume + direct destination writes). The
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

## Investigate GCC LTO DWARF emission on ELF (Linux `.so` reads 0 DIEs)

**When:** whenever the Linux SIL flavor wants LTO's ~30% firmware speedup back
(today it runs `-O3` without `-flto`, so it is only ~30% slower on the firmware
half of a step — a non-blocking gap).

**What we saw (PR #2, run 3):** the release SIL DLL built with `-O3 -flto
-ffat-lto-objects -g` links fine on Linux (GNU gcc), the `.so` loads, and its
250 exports resolve — but gimli parses **0 DWARF variables and 0 DWARF
functions** from it (`no usable ASLR anchor ... 250 exports, 0 DWARF variables,
0 DWARF functions`). The identical flags on Windows (MinGW, same GCC major)
produce a clean DWARF map. So GCC's LTO code-gen is emitting DWARF that gimli
cannot read on ELF (or emitting none for the merged program), even with
`-ffat-lto-objects`.

**Interim fix (2026-07-10):** LTO is gated to Windows only (`CMAKE_HOST_WIN32` in
`sw/cmake/toolchains/native.cmake`); Linux/macOS release keeps `-O3` without
`-flto`, which reads clean. macOS never had LTO (Apple clang lacks
`-ffat-lto-objects`).

**To investigate:** whether GCC-on-ELF needs `-ffat-lto-objects` *plus* a
`-flto`-aware objcopy/debug-link step; whether the fat objects' `.debug_*`
survive the LTO link or are dropped for the GIMPLE'd units; whether `-flto=auto`
+ `-g3` or `-gdwarf-4` changes what lands in the `.so`; and whether gimli needs
the fat-object debug sections pointed at explicitly. Compare a `-flto` vs
non-`-flto` `.so`'s `.debug_info` with `readelf --debug-dump=info`.

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
