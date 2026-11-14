# SIL backlog

Deferred cleanup tasks — parked here so they aren't lost, with enough scope
detail to pick up cold. Not roadmap items (see `roadmap.md` for those).

## hw: sync the design files with the as-built board (Y1 = 24 MHz)

**When:** next hardware-design touch. The assembled board carries a 24 MHz
crystal at Y1 — `HSE_VALUE = 24000000` is correct and silicon runs 144 MHz;
the schematic (`hw/micro.kicad_sch`) and BOM still say 25 MHz. As-built
deviations live in `hw/rework-log.md` — update the schematic/BOM to match,
and log any future rework there.

## sim: the injected dispatch sequence batches differently under a coarse grid

**When:** whenever the sim ADC completion path or the engine grid is next
touched, or before a test relies on one dispatch meaning one trigger.

`HW_ADC`'s injected dispatch sequence identifies a trigger by its interrupt
entry: stm32g4 bumps it once at the top of `HW_ADC_irqHandler` before the
per-channel loop, and the sim bumps it once at the top of
`HW_ADC_sim_completionDispatch` before the drain, so completions serviced
together share a value on both targets. `IO_bridge` pairs U and V on it to
derive phase W.

Two sim-only departures, neither reachable at the normal 50 us cadence:

- An entry that drains nothing still consumes a sequence. Harmless — a
  sequence identifies an entry, not a sample.
- If `pendingCompletions[ch]` exceeds 1, the sim fires several callbacks for
  one channel under a single sequence, where hardware would have used a
  separate entry per trigger. This needs a dispatch deferred past a trigger,
  i.e. a grid coarse enough to queue completions.

The second one matters if a coarse-grid scenario ever pairs on the sequence:
two triggers' samples would share an identity and pair wrongly. Either drain
one completion per sequence, or assert the queue depth stays at 1 on the
grids the suites use.

## SIL: no perf regression gate — the µs/step numbers cannot regress detectably

**When:** soon. Every lever in `performance.md` is protected only by a doc
paragraph.

**What:** CI runs `tools/run_sil.sh`, which runs the perf binary as step 4/4
and fails only on a non-zero exit. Every number it prints goes to stdout and is
discarded — there is no committed baseline, no tolerance, and no machine
record, so a 3× regression lands green.

**Shape of the fix (deliberately cheap):** a normal `#[test]` in
`sw/sil/pcs_bldc_sil/tests/` that warms up a board world on the 50 µs grid,
times N steps, and asserts a **loose** ceiling — roughly 3× the documented
typical, which is still well clear of shared-runner noise. It catches the
structural class of regression (an allocation or a string key back on a hot
path), not a few percent of drift. A tight wall-clock tolerance would flake and
is not worth having. Worth pairing with a deterministic companion that asserts
the *mechanism* the perf rests on — motor advances once per step, encoders zero
times, sense only on a changed input, sweep once per `sweep_period_us` —
which cannot flake at all; `grid.rs` already asserts the sweep half.

## SIL: the per-member perf attribution instrument is invalid

**When:** whenever someone next wants a per-member cost split. Until then the
rows are informational and `performance.md` says so.

**What:** `report_board_world` in `pcs_bldc_sil/src/main.rs` attributes each
member's cost as `full − (world with that member disabled)`. That subtraction
is only valid if disabling a member removes its cost and nothing else, which is
false here: the encoders sit on the firmware's SPI bus, so disabling one
changes what the firmware executes. The dial-encoder row reads
**−11.76 µs of a 3.11 µs step** — disabling it makes the world ~5× *slower*,
presumably because the firmware retries on the dark bus. `report_performance`
has the same defect one report up (`of which model+route+propagate` goes
negative on the same run).

**Fix, in preference order:** (a) delete the per-member loop and keep only the
`full board world` row, which is a valid end-to-end measurement; or (b) if
per-member attribution is genuinely wanted, take it from a scoped timer around
each member's `advance` inside the engine — which already sequences them —
rather than from world-vs-world subtraction, and clamp or flag a negative row
instead of printing it as fact.

## bench: `pwm_isense_sampling.ipynb` is committed with its outputs embedded

**When:** hygiene — next touch of `tools/trace_analysis/`.

**What:** the other trace_analysis notebooks are committed output-stripped;
`pwm_isense/pwm_isense_sampling.ipynb` is not — ~458 KB of its 477 KB is
base64 PNG output. Nothing enforces the convention: there is no `nbstripout`
config and no pre-commit hook.

**Fix:** either clear it (`jupyter nbconvert --clear-output`) and add an
`nbstripout`/pre-commit config so the convention holds by construction, or
decide deliberately that this notebook is a rendered artifact and say so in its
own header cell.

## sim: move the GPIO EXTI detector onto the SIL_irq path

**When:** when anything needs an input edge faster than 1 kHz, or opportunistic
cleanup — whichever comes first.

**What:** sim `HW_GPIO` detects EXTI edges by comparing injected input levels
inside `HW_GPIO_run1ms`, counting them in `extiEdgeCount`. Every other sim
module that models an interrupt registers with the framework interrupt
table (the pended-completion pattern: ADC, SPI, DMA, USB), so GPIO is the odd
one out — and a 1 ms poll structurally cannot represent an edge faster than
1 kHz, nor place one anywhere but on a millisecond boundary.

**Fix:** register the EXTI handler as a pended entry and pend it from the
level-injection path, so an edge dispatches at the sim instant it happens and
at its configured priority. The `line_asserted` level extension sketched in
`sim-interrupts.md` §6 is the natural follow-on for level-sensitive lines
(nFAULT/BKIN), not a prerequisite here.

## Motor model: raise the integrator's realtime ceiling (remaining levers)

**When:** when a long single scenario or the Phase-4 fast-mode/pytest sweeps
actually need it; owner-led (owner physics).

**Why:** the plant is the single largest member cost in a board world —
~2.4 us of a ~4.8-5.3 us step on the 50 us grid — and it scales with sim
time, not step count, so it is the term that decides how much faster than
realtime a long run can go.

**Spent lever:** flat coarsening. The semi-implicit-Euler sub-step is 5 us
(200 sub-steps per ms), which is where the board world's ~10x realtime comes
from. There is no more headroom here: the accuracy limit is the
explicitly-forced BEMF term, so error scales with omega_e*dt, and 10 us
already drifts 30% at 2 kHz electrical against a 1 us reference
(`performance.md` §16). Do not coarsen further without changing the scheme.

**Levers, in leverage order:**
- **Dynamic/adaptive sub-step:** the fine step guards the stiff diode-mode
  transitions (engagement/anti-chatter), not the RL dynamics (tau_e = L/R is
  340 us). Fine steps only around diode-mode changes, coarse elsewhere.
  Step selection must be state-dependent only (determinism). The analytic
  `motor_dynamics.rs` asserts + MF4 diffs against 1 us runs are the accuracy
  instruments.
- **Per-sub-step cost:** ~700 cycles for 3-phase electrical + mechanics +
  diode logic; maybe 2-3x from optimization (branch shape, layout, math).
- **Integrator configuration:** a stiffly-stable scheme (exponential /
  implicit) for the linear RL part tolerates much larger steps; biggest
  rewrite, same accuracy questions, composes with the adaptive lever.

## fw: alignment dwell captures a mid-swing offset (owner design call)

**When:** ready for the owner's design call — the plant carries the real
motor's parameters (J 1.45e-4 kg·m², viscous 1e-4 Nm·s/rad, Coulomb 15 mNm,
cogging), which was the precondition.

**What (found by `tests/north_star.rs`):** the alignment spring is a lightly
damped oscillator, so the fixed 500 ms dwell can end mid-swing and capture an
offset off the true equilibrium while the rotor still moves; the effective
commutation lead then lands outside the designed 60..120° and the drive
delivers a fraction of the available torque. The severity on the real plant
(Coulomb friction now damps the swing) is to be re-measured. Candidate fixes,
owner's choice: settle detection (capture when |velocity| under a floor, dwell
as timeout), a longer dwell, a stronger/ramped alignment duty, or a two-step
align (coarse + settle).

## voyant: declarative transform routes (stateless conversions on the wire)

**When:** once 2-3 stateless conversions accumulate (candidates: vsense divider
ratio, encoder mounting offset, gear ratios), or when Phase-4 Python-defined
wiring lands — whichever comes first. Not needed for the current-sense model
(that chain carries state — noise, saturation — so it is a Member).

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

- **Exact diode-pair engagement.** The window check is per-terminal with a
  `v_n = 0` all-open convention, so it "clamps" the most-negative phase even
  though a single diode into an isolated wye has no return path. The clamp
  carries ~zero current (KCL), so dynamics are untouched, but the reported
  `terminal_voltage_*`/`neutral_voltage` are convention-colored: during a
  sub-window coast `v_n` reads `Ke·ω − v_d` with 0-V blips at clamp handover.
  Fix: engage diodes pairwise on the line-to-line condition; below it, no leg
  clamps and the all-open convention takes over honestly.
- **Divider-defined float level (vsense model).** Physically the all-open
  terminal potentials vs ground are set by the vsense divider network, not the
  machine (bench: dark bridge floats ~7 V together). The motor model's
  convention can stay neutral; the vsense model owns mapping true terminal
  state → what the ADC sees, including the float level.

## Decoded telemetry as table signals

**When:** when a scenario wants to assert on, or trace, a telemetry field over
time.

**What:** tests read the sim USB TX capture by DWARF (`read_tx_capture` in
`sw/sil/pcs_bldc_sil/src/wire.rs`) and decode the protobuf frames after the
fact. What remains is decoding them into the State Table as signals (e.g. a
`usb:pcs_bldc:velocity_measured_radps` entry), so a test asserts on a named
signal and the historian and MF4 export carry the device's own view. Design
anchor: `state-route-tables.md` §1 "Comms entries".

## Encoder sensor noise — mode-hold assert in the zero-demand window

The board world runs both AS5048s noisy (`As5048Model::with_noise`), and
`tests/north_star.rs` (`dial_demand_spins_rotor_closed_loop`) asserts `TIM1_MOE`
holds and the phases stay idle across a zero-demand window. Missing: an assert
that the drive mode holds across that same window — the bench condition that
exposed the mode-flapping bug.

## Enum cvars mirror as DWARF placeholders (`<0>`, `<1>`), not enumerator names

**When:** near-term — it degrades trace readability (every enum channel in an
`.mf4` renders `<n>` instead of e.g. `HW_ADC_TRIGGER_SOFTWARE`) and FSM
asserts will want symbolic mode names.

**What:** firmware enum channels in a trace resolve to the backend's
placeholder form (`<0>`, `<1>`, …) — the DWARF enumerator-name resolution
isn't producing names for mirrored enum leaves (e.g.
`cvar:pcs_bldc:HW_ADC_channelConfig[*].triggerMode`). The trace pipeline is
not the bug — it carries whatever the mirror records, and the MDF
value-to-text mechanism works with real names in isolation. Investigate the
backend's enum decode on the mirror path (typed-lane `ScalarSample::Boxed`
vs `dwarf.rs` value→name lookup). Suite check 3 only asserts
`matches!(.., Value::Enum(_))` — the variant, not a symbolic name — so it
cannot catch this; strengthen it once fixed.

## DuplexTransfer — residual extensions

**What:** the synchronous member↔member serial-transaction primitive serves
`spi`. Remaining:

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
  cannot answer another synchronously. This rides the delayed-response
  extension below, not the synchronous path.

**Delayed duplex responses via the D8 interrupt table.** The firmware
initiates a transfer; the peer callback parses TX, runs arbitrary code, loads
the response buffer, *and configures the response interrupt* (delay +
handler) — letting the framework deliver the response at a future sim time
with roughly-modeled transfer delays. Pursue only if precision-delayed duplex
transfers buy real timing accuracy. Aligns with `sim-interrupts.md` §3's
example (sim `HW_SPI` scheduling a one-shot `SPI3_IRQHandler` 2 µs out); the
new element is the *peer* configuring the delay/response rather than the
driver hardcoding a literal. Natural fit: non-blocking (DMA/IT) buses where
real hardware raises a completion IRQ after bytes × bit-time.

## Remove the last `HW_<Module>_sim.h`: USB

**What remains:** `sw/lib/c/shared/hw/USB/sim/HW_USB_sim.h` is the one
inject/inspect header left under `sw/lib/c/shared/hw/`. Its consumers are
three Unity suites — `hw/USB/sim/test/test_HW_USB.c`,
`io/COBSFrame/test/test_IO_COBSFrame.c` and
`app/server/test/test_app_server.c` — plus the sim driver that implements
it. SIL already drives and reads the sim USB through `HW_USB_sim_data` by
DWARF. Remove it the way the other drivers went: state as DWARF-visible data
fields, the Unity suites rewritten against a test-owned double (the
`mock_HW_USB` that `io/serial`'s tests use is the pattern), the header
deleted. Then add a lint that fails on any `*_sim.h` under `sw/lib/c/shared/hw/`
(the `HW_<M>_sim_completionDispatch` ISR entries are not inject/inspect APIs,
so the lint checks files, not the `_sim_` prefix).

**Policy:** do not add new consumers of `_sim_*` APIs (in C, Rust, or
scripts). SIL-side injection/inspection goes through DWARF read/write of the
sim drivers' statics and Route Table suspend + direct destination writes.

## Investigate GCC LTO DWARF emission on ELF (Linux `.so` reads 0 DIEs)

**When:** whenever the Linux SIL flavor wants LTO's ~30% firmware speedup back
(it runs `-O3` without `-flto`, so it is only ~30% slower on the firmware
half of a step — a non-blocking gap).

**What:** the release SIL library built with `-O3 -flto -ffat-lto-objects -g`
links fine on Linux (GNU gcc), the `.so` loads, and its exports resolve — but
gimli parses **0 DWARF variables and 0 DWARF functions** from it. The
identical flags on Windows (MinGW, same GCC major) produce a clean DWARF map.
So GCC's LTO code-gen emits DWARF that gimli cannot read on ELF (or none for
the merged program), even with `-ffat-lto-objects`. LTO is gated to Windows
(`CMAKE_HOST_WIN32` in `sw/cmake/toolchains/native.cmake`); Linux/macOS
release keep `-O3` without `-flto`. macOS has no LTO (Apple clang lacks
`-ffat-lto-objects`).

**To investigate:** whether GCC-on-ELF needs `-ffat-lto-objects` *plus* a
`-flto`-aware objcopy/debug-link step; whether the fat objects' `.debug_*`
survive the LTO link or are dropped for the GIMPLE'd units; whether `-flto=auto`
+ `-g3` or `-gdwarf-4` changes what lands in the `.so`; and whether gimli needs
the fat-object debug sections pointed at explicitly. Compare a `-flto` vs
non-`-flto` `.so`'s `.debug_info` with `readelf --debug-dump=info`.

## Future feature: native debugger (VSCode) attached to in-the-loop firmware

**Idea:** run voyant with firmware in the loop, connected to the VSCode
debugger — breakpoints, stepping, variable watches in the C firmware source
while the sim runs. Realtime-sim flavored: fine to build the firmware at
`-O0 -g` (the `run_sil.sh --debug` flavor).

**Feasibility sketch:** most of this comes free from the architecture. The
firmware is a native DLL inside the voyant process, so attaching a native
debugger (MinGW gdb / VSCode `cppdbg`) to that process gives source-level
breakpoints/stepping/watches in the C — the DLL carries full DWARF, and gdb
shows the executing fiber's stack at a breakpoint. The sim is single-threaded:
**hitting a breakpoint freezes the entire virtual world coherently** — motor
model, sim time, everything — unlike real hardware, where the plant keeps
moving while the CPU is halted; and determinism means you can replay to the
same breakpoint identically.

**To productize (rough):** a `--wait-debugger` / hold mode in the driver +
VSCode `launch.json` attach configs; a pacing policy that tolerates wall-clock
stalls (a breakpoint stalls the paced loop — resync rather than sprint on
resume); docs. Natural host is the **realtime/paced run mode**, so this
sequences after fast mode + Python bindings (D3) and the realtime dashboard
(D4).

## Sim USB: packetize the host-to-device stream

**When:** before the next protocol change that grows a request, or the next
TinyUSB upgrade.

**What it is:** the sim USB hands the firmware whatever bytes a check
injected, so a frame that is an exact multiple of the 64 B full-speed bulk
packet looks like any other. On the board such a frame has no short packet
behind it and a multi-packet OUT transfer does not complete without the
vendored TinyUSB patch ("USB link: double-buffered bulk IN",
`docs/backlog.md`). Model the packet boundary in the sim USB: deliver
injected bytes in 64 B packets and apply the transfer-completion rule the DCD
uses, so a `fast_trace` scenario sending a 64 B request reproduces the stall
when the patch is absent.

## Current sense: gate the sense model on the low-side on-state

The SIL current-sense model reports phase current regardless of switch state;
the board's low-side shunts see it only while the low side conducts
(`tools/trace_analysis/stall_current_measure_r` shows the `(1−duty)·I` mean
under asynchronous sampling). This is the last step of the order in "Low-side
shunt window at the voltage ceiling" (`docs/backlog.md`): bench scope check,
then the firmware choice, then gate this model to match.

## Encoder: AS5048 propagation delay

`sw/sil/pcs_bldc_sil/src/as5048.rs` reports the angle with no delay; the
AS5048 reports about 100 µs late (datasheet p. 9). Model it so the estimator's
latency terms read in SIL as on the bench. The firmware side is the
"Encoder-latency compensation" open design item in `docs/motor-sprint.md`.

## Inverter: dead-time distortion

The SIL inverter carries no dead-time voltage error. On the bench it shows as
a 6/12/24-per-electrical-cycle velocity ripple at crawl speeds
(`tools/trace_analysis/estimator_order`). The sim timer holds each phase's
two line edges with the dead-time (`HW_TIM_data.peripheralData[].primaryLine`
/ `.complementLine`); the plant does not consume them.

## bench: hung-weight run for T̂_d

A known static load (a weight on a string from the shaft) read against T̂_d.
The steady-speed torque balance covers its calibration; the static-load case
is unmeasured. The SIL load-torque port (`motor.rs`) stands in meanwhile.
