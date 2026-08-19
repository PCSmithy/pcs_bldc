# SIL backlog

Deferred cleanup tasks — parked here so they aren't lost, with enough scope
detail to pick up cold. Not roadmap items (see `roadmap.md` for those).

## hw: sync the design files with the as-built board (Y1 = 24 MHz)

**When:** next hardware-design touch. Owner confirmed 2026-08-16: the
assembled board carries a 24 MHz crystal at Y1 (assembly-time substitution)
— `HSE_VALUE = 24000000` is CORRECT and silicon runs 144 MHz; it is the
schematic/BOM that are stale (they say 25 MHz). As-built deviations now
live in `hw/rework-log.md` — update the schematic/BOM to match, and log any
future rework there.

**Related fw follow-up (owner-directed):** TIM1's `.period = 4249` was
sized for a 170 MHz clock (20 kHz center-aligned); at the real 144 MHz it
gives 16.94 kHz. ARR 3600 restores exactly 20.000 kHz / 50 µs. Lands with
the owner's injected-ADC CubeMX work.

## Motor model: raise the ~4.5x-realtime ceiling (integrator levers)

**When:** when a long single scenario or the Phase-4 fast-mode/pytest sweeps
actually need it; owner-led (owner physics). Not this sprint.

**Why (measured 2026-08-15):** the plant costs ~220 us wall per 1 ms of sim
time — 1000 x 1 us semi-implicit-Euler sub-steps at ~0.22 us (~700 cycles)
each — capping any board world at ~4.5x realtime on every grid. The
framework's own fine-grid cost is ~2.3 us/step (22x+); the plant is the
floor. Largely hidden today by nextest's process-per-test parallelism.

**Levers, in leverage order:**
- **Dynamic/adaptive sub-step:** the 1 us step guards the stiff diode-mode
  transitions (engagement/anti-chatter), not the RL dynamics (tau_e = L/R is
  hundreds of us). Fine steps only around diode-mode changes, coarse
  elsewhere — or a measured flat coarsening to 5-10 us — plausibly 5-10x.
  Step selection must be state-dependent only (determinism). The analytic
  `motor_dynamics.rs` asserts + MF4 diffs against 1 us runs are the accuracy
  instruments.
- **Per-sub-step cost:** ~700 cycles for 3-phase electrical + mechanics +
  diode logic; maybe 2-3x from optimization (branch shape, layout, math).
- **Integrator configuration:** a stiffly-stable scheme (exponential /
  implicit) for the linear RL part tolerates much larger steps; biggest
  rewrite, same accuracy questions, composes with the adaptive lever.

## One API header per module — io layer (owner scoping TBD)

**When:** owner call. The `hw/` layer is done (2026-08-15): each dual-target
module now has one `hw/<Module>/HW_<Module>.h` carrying the API + shared
types, with `<target>/HW_<Module>_target.h` holding only the config shape
(see the channelization section of `c-coding-conventions.md`). Whether the
io-layer modules want the same treatment is unscoped.

## Current sense: model the low-side-shunt duty visibility (bench-confirmed)

**When:** before matching sim phase-current traces against bench captures at
speed, or when the firmware moves to PWM-synchronized (injected) ADC sampling
— whichever forces it first.

**What (bench finding, 2026-08-09, stall-R capture):** the board's shunts are
low-side, so a leg's shunt sees winding current only while its low-side path
conducts. Under the current asynchronous 1 ms ADC sampling the PWM'd leg's
*mean* reading is ≈ `(1−duty)·I` while a tied-low leg reads true `I` —
confirmed at every duty level (`tools/trace_analysis/stall_current_measure_r`).
Consequences: (a) the SIL `CurrentSenseModel` feeds true currents, so sim and
bench current telemetry diverge on PWM'd legs by design — matching needs a
duty-visibility term (inputs exist: the model can observe the bridge command
like the motor does); (b) the firmware's overcurrent protection under-reads
PWM'd legs by the same factor (a 0.9-duty leg reads ~10% of its current —
six-step is covered because the tied-low partner reads true, but FOC/SVPWM
PWMs all legs); the real fix is PWM-valley injected sampling — exactly the
reserved `fw~hal_adc_003`/`fw~hal_adc_008` motor-sprint work.

## fw: alignment dwell captures a mid-swing offset (owner design call)

**When:** after the plant params are ballparked from the real motor — the
severity depends on the true J/B/spring; decide then between firmware fixes.

**What (found by `tests/north_star.rs`, 2026-08-09):** the alignment spring on
the placeholder plant is a ζ≈0.05, ωn≈10 rad/s oscillator (swing period
~0.63 s, envelope τ = 2J/B = 2 s), so the fixed 500 ms dwell ends mid-swing:
offset captured 76.7° electrical off the true equilibrium while the rotor
still moves at +0.63 rad/s. Effective commutation lead lands at 136..197°
(mean 167°) against the designed 60..120° — the drive spins but delivers
~37% of available torque (terminal 25 rad/s vs the ideal 67.5). Candidate
fixes, owner's choice: settle detection (capture when |velocity| under a
floor, dwell as timeout), a longer dwell, a stronger/ramped alignment duty,
or a two-step align (coarse + settle). The zero-demand shorted-pair braking
(`app_motorControl.c` else-branch) adds negligible damping (~6e-6 vs
B=1e-4 Nm·s/rad) and does not rescue the dwell.

## voyant: declarative transform routes (stateless conversions on the wire)

**When:** once 2-3 stateless conversions accumulate (candidates: vsense divider
ratio, encoder mounting offset, gear ratios), or when Phase-4 Python-defined
wiring lands — whichever comes first. Not needed for the current-sense model
(that chain grows state — noise, saturation — so it is a Member; ruling
2026-08-07).

**What:** let a route carry a *declarative* value transform applied during
propagate — data, not code: e.g. `Affine { gain, offset }` and `Clamp { lo,
hi }`, composable, NOT `Box<dyn Fn>`. Constraints: stateless and pure (D7);
applied identically on the delayed and zero-latency paths; the destination
records the transformed value (the historian shows what the consumer saw);
the transform is part of route registration metadata so traces and the
(future) Python bindings can express and inspect it as plain data. Anything
needing state (RNG, filters) stays a Member — the transform seam must refuse
that scope creep.

## Motor model: undriven terminal voltages are approximate below the diode window

**When:** the vsense hardware-matching work (comparing sim terminal voltages
against phase-vsense bench captures) — the coast segments will disagree with
hardware by construction until both pieces land.

**What:** two related refinements, both about undriven legs while line-to-line
BEMF is below the conduction window (`e_max − e_min < vbus + 2·v_d`):

- **Exact diode-pair engagement.** The step-5 window check is per-terminal
  with a `v_n = 0` all-open convention, so it "clamps" the most-negative
  phase even though a single diode into an isolated wye has no return path.
  The clamp carries ~zero current (KCL), so dynamics are untouched, but the
  reported `terminal_voltage_*`/`neutral_voltage` are convention-colored:
  during a sub-window coast `v_n` reads `Ke·ω − v_d` with 0-V blips at clamp
  handover (verified against `six_step_spins_then_coasts_at_tau_mech.mf4`,
  2026-08-07). Fix: engage diodes pairwise on the line-to-line condition;
  below it, no leg clamps and the all-open convention takes over honestly.
- **Divider-defined float level (vsense model, stage 6).** Physically the
  all-open terminal potentials vs ground are set by the vsense divider
  network, not the machine (bench: dark bridge floats ~7 V together). The
  motor model's convention can stay neutral; the vsense model owns mapping
  true terminal state → what the ADC sees, including the float level.

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

## Encoder sensor noise — mode-flapping regression scenario (remaining piece)

**Why (owner observation, 2026-07-29):** real AS5048s never read a static 0 —
the dial's noise floor made the mode-flapping bug manifest *immediately* on
hardware (LED flicker), while the noiseless sim encoder surfaced the same bug
as alignment latency instead. A model without noise cannot reproduce
noise-triggered failure modes.

**Done (2026-08-01):** characterization (baseline notebook, deconvolved
sigma 1.52/1.22 LSB, Gaussian + white), the model
(`As5048Model::with_noise(sigma_lsb, seed)`, `sw/lib/rust/prng` SplitMix64 +
CLT-12 — pure-arithmetic Gaussian, deterministic per D7; default noiseless,
per-instance seeds), and the statistical harness (`tests/encoder_noise.rs`
at the SPI boundary; prng tests run standalone from the crate dir, not via
`run_sil.sh` — owner ruling). Alignment runs both encoders noisy.

**Remaining:**
- **Regression scenario (harness-side):** noisy dial hovering at zero, armed —
  asserts `TIM1_MOE` and the mode hold rock-solid across the noisy window:
  the sim reproduction of the bench condition that exposed the flapping bug,
  guarding the 31fab7c firmware fix.
- Deferred, deliberate: a firmware-side deadband on the dial accumulator if
  zero-adjacent `velocityRequest` jitter ever matters downstream.

## Fiber port: macOS un-convert teardown

**When:** with the macOS ucontext/asm port, if one lands. The Windows fiber
port fully supports repeated boots and shared threads; the macOS equivalent
needs the same `vPortEndScheduler` un-convert teardown when it exists.

## Enum cvars mirror as DWARF placeholders (`<0>`, `<1>`), not enumerator names

**When:** near-term — it degrades trace readability (every enum channel in an
`.mf4` renders `<n>` instead of e.g. `HW_ADC_TRIGGER_SOFTWARE`) and stage-7
FSM asserts will want symbolic mode names.

**What (found by MF4 round-trip validation, 2026-07-28):** all 106 firmware
enum channels in a trace resolve to the backend's placeholder form (`<0>`,
`<1>`, …) — the DWARF enumerator-name resolution isn't producing names for
mirrored enum leaves in this build (e.g.
`cvar:pcs_bldc:HW_ADC_channelConfig[*].triggerMode`). The trace pipeline is
NOT the bug — it faithfully carries whatever the mirror records, and the MDF
value-to-text mechanism is proven with real names in isolation. Investigate
the backend's enum decode on the mirror path (typed-lane `ScalarSample::Boxed`
vs `dwarf.rs` value→name lookup). Note: suite check 3 only asserts
`matches!(.., Value::Enum(_))` — the variant, not a symbolic name — so it
cannot catch this; strengthen it once fixed.

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

- `sw/lib/c/shared/hw/DMA/sim/HW_DMA_sim.h`
- `sw/lib/c/shared/hw/GPIO/sim/HW_GPIO_sim.h`
- `sw/lib/c/shared/hw/OPAMP/sim/HW_OPAMP_sim.h`
- `sw/lib/c/shared/hw/SPI/sim/HW_SPI_sim.h` (SPI's `setInjectedRx` is already
  gone — DuplexTransfer replaced it and the Unity suite was rewritten against a
  test-owned hooks double; the remaining `_sim_*` getters here still apply)
- `sw/lib/c/shared/hw/TIM/sim/HW_TIM_sim.h`

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
- ☑ **USB** — DONE 2026-08-13: `io/serial` tests run on a boundary
  `mock_HW_USB`, the sim-driver suite is retired (sim drivers are
  framework-covered via SIL), `HW_USB_sim.h` deleted. `fw~hal_usb_004`
  (CDC RX) joins the defect baseline until a firmware RX consumer exists
  (desktop-app protocol); the `usb_cdc` sig_type item above stands on its
  own for telemetry-as-signals.
- ☑ **ADC** — DONE 2026-08-18: conversion-stall injection and multimode
  inspection move to SIL DWARF write/read (`tests/adc_faults.rs`); sim
  `HW_ADC_init` is re-entrant, so the Unity suite's clean slate is a rejected
  init and the config-rejection / readout-guard tests stay there;
  `HW_ADC_sim.h` deleted.
- ☐ **DMA**, **OPAMP** — final sweep after sprint stage 7: pick
  per-capability replacements (test-owned hooks double or DWARF write),
  rewrite/retire the suites, delete the headers.
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
