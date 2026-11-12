# Motor sprint — drive path, FOC current loop, velocity/position control

The successor to [`docs/foundation-sprint.md`](foundation-sprint.md) (now
complete). The foundation sprint delivered a spec'd + traced driver stack and
bench-verified board sensing; this sprint turns it into a spinning,
closed-loop, field-oriented motor controller. SIL-first: the motor model in
the `sim` target is developed alongside, so controllers and estimators are
validated against the simulation before the bench.

## [START HERE] — entry point

**Where we are:** foundation sprint done (M1–M5); Milestone 1 largely
done. Full hw/io/dev/app driver stack, all 10 analog inputs bench-verified,
FreeRTOS task architecture (`task_1ms` > `task_10ms` > `task_usb` >
`server`) running. First motion is real: six-step commutation
(`fw~mc_011`/`fw~mc_012`) with overcurrent and encoder-fault protection,
phase currents from the 20 kHz injected ADC with a per-cycle callback
(`fw~io_bridge_006`/`_007`), and a 20 kHz signal trace to the desktop app.
`IO_bridge` and `app_motorControl` are C++ classes
(`docs/cpp-coding-conventions.md`); new control code is C++. The transforms
and modulator (`fw~mc_013`/`_014`), the bridge's voltage-vector command
(`fw~io_bridge_008`), the PWM-synchronous step (`fw~mc_015`) and the
rotor-frame current observation (`fw~mc_016`), the sensored sinusoidal
method (`fw~mc_020`) on the per-cycle rotor angle (`fw~mc_019`), and the
bootstrap precharge (`fw~mc_021`) are in, as is the encoder phase-locked
rotor tracking estimator `fw~est_rotor_001`/`_002` (`app_rotorEstimation`,
`specs/firmware/est/rotor.md`) that `fw~mc_019` and the status velocity
consume, and the bridge's zero-current offset tracking (`fw~io_bridge_009`).
On the `mc_foc` branch, bench-spun, with its plant-level step responses on
the SIL's held rotor (`tests/foc.rs`): the field-oriented torque method,
its current regulator and voltage limit (`fw~mc_022`..`fw~mc_024`,
`specs/firmware/mc/foc.md`, `lib_pi` + `MODE_FOC_TORQUE` in `app_motorControl`).
Built, its step-response tests next: the field-oriented velocity method
`fw~mc_025` (same file), a per-cycle velocity PI wrapping the torque method
as its own selectable mode (`MODE_FOC_VELOCITY`). Deferred: the V/f method `fw~mc_010`/`fw~mc_017`
(`specs/firmware/mc/vf-sinusoidal.md`) is the sensorless start-up ramp,
scheduled with sensorless FOC under Milestone 3.

**Read first:**
1. `CLAUDE.md`, plus the ADC and unhandled-IRQ gotchas at the bottom of this
   file.
2. `specs/system/mc/motor-control.md` (`sys~mc_001`, the sprint's system
   anchor) + `specs/firmware/hal/adc.md` (`fw~hal_adc_003` / `fw~hal_adc_008`
   are the reserved timer-triggered-injected + async-completion specs).
3. `specs/firmware/hal/tim.md` (`HW_TIM`) — the base that Milestone 1 extends
   for complementary PWM.

**Workflow** is unchanged from the foundation sprint: use the `pcs_spec`
skill to author/extend specs (interview → two research agents → draft →
blind style audit → write + tag + test), keep native ctest and the ARM link
green, and hold OFT at no dangling tags. New parents live under `sys~mc_001`;
estimation work opens the first `est` topic specs.

## Milestones (critical path: 1 → 2 → 3)

### 1. Drive-path bring-up

First motion, open-loop, at low voltage/current. Proves the gate-driver
control path and the protective plumbing before any closed loop.

- **`HW_TIM` extension** for TIM1: 3-phase complementary PWM (CH1/2/3 +
  CH1N/2N/3N) with programmable dead-time, center-aligned, plus the update
  event that will later trigger ADC sampling. Amend `specs/firmware/hal/tim.md`.
- **STSPIN32G4 configuration + fault handling.** Bring up the integrated gate
  driver (enable, gate-drive settings, dead-time coherence with TIM1),
  surface and latch its fault/nFAULT line. New `hal`/`drivers`-topic spec(s)
  under `sys~mc_001`.
- **Open-loop commutation.** Six-step first spins at reduced bus voltage /
  current-limited, confirming phase ordering, sense polarity, and
  mechanical rotation. Done.
- **Sensored sinusoidal commutation** (`fw~mc_020`, voltage-mode on the
  per-cycle rotor angle `fw~mc_019`): the SVM path on the bench, and the
  d/q frame FOC closes its loops in.
- **Software overcurrent trip** off the already-verified phase-current sense
  channels (INA240 phase I, VBUS I): a fast poll/ISR threshold that disables
  the bridge. This is the standing safety net for everything after.

### 2. FOC current loop

Sensored inner torque loop, SIL-first, using the U/V ADC-instance split
already in place.

- **Timer-triggered injected sampling** — done: TIM1-triggered injected
  conversions on ADC1 (U) and ADC2 (V) with the per-cycle callback
  (`fw~hal_adc_010`, `fw~io_bridge_006`/`_007`).
- **Zero-current offset tracking** — done (`fw~io_bridge_009`): the bridge's
  EMA of each idle phase's reading, latched when the phase next drives.
- **Clarke / Park transforms, SVM** — done (`fw~mc_013`/`_014`,
  `fw~io_bridge_008`).
- **PI current loops (Id/Iq)** — done on `mc_foc` (`fw~mc_022`..`fw~mc_024`):
  `lib_pi` regulators with K_p = L·ω_bw / K_i = R·ω_bw, vector limit at
  min(V_max, V_bus/√3) with back-calculation anti-windup, i_q* from the
  speed-target fraction times I_max; MODE_FOC_TORQUE plumbed like MODE_SINUSOIDAL.
  Board config seeds R = 14.7 Ω, L = 5 mH from the SIL motor model,
  ω_bw = 1000 rad/s, I_max = 1 A. Bench-spun; the step response and the limit
  hold on the SIL's held rotor (`tests/foc.rs`).
- **Encoder electrical-angle alignment** — done (`fw~mc_012`).
- **Control task** driven by the conversion-complete ISR — done
  (`fw~mc_015`).
- **SIL:** motor electrical model with a rotor lock (`MotorParams::rotor_locked`);
  the FOC step responses run on the 50 µs grid against it. A sense-bias
  parameter for a plant-level offset test is still open. The closed-loop
  tracking run is the first `[test->sys~mc_001~1]`.

### 3. Velocity / position control + estimation

Outer loops and the first estimator work.

- **Host command plane** — in build, for the step-response
  test plan behind the gain and estimator decisions: one `Command` carrying
  a mode and a timed setpoint list the firmware plays on its 1 ms tick
  (`fw~conn_server_007`, `fw~ops_001`/`_002`), a torque target in Nm for
  the torque method (`fw~mc_026`, `fw~mc_022` edited), a setpoint rate
  limit (`fw~mc_027`, built after the plane), the motor rating report
  (`fw~conn_server_008`), and the app's Command and Sequence widgets with
  a capture export (`app~conn_004`, `app~views_024`..`_028`). `SetMode`
  and `SetVelocity` retired with it. Built to take a new
  setpoint kind without restructuring: the `Setpoint` oneof, one per-kind
  write in the sequencer, one target struct per kind in motor control, a
  per-kind bound in `MotorInfo`, one per-kind unit/bound row in the widgets.
- **Position control** closes the sprint, specced and built 2026-11-10
  (bench verification pending; SIL: a 2π move lands within 1 mrad, a
  10 mNm load at rest moves the rotor 2 mrad): the
  position method `fw~mc_029` cascaded around the velocity method (a PI
  on the rotor position `fw~mc_032` with the move profile's velocity fed
  forward), the position setpoint `fw~mc_030` and the on-board move
  profile `fw~mc_031` (a minimum-time trapezoid under per-setpoint
  velocity and acceleration limits, a velocity at the point for chained
  moves, a jog from the speed target), the position arm through the
  command plane (`fw~conn_server_007`/`_008`, `fw~ops_001`,
  `app~conn_004`, `app~views_024`/`_025`/`_027`) and Telemetry
  (`fw~obs_status_001`). Settled: a multi-turn position, zero at the
  bridge enable, bounded to ±10 turns; profiles generated on the board
  from the sparse points, the sequence chart drawing the same profile;
  absolute `t_us` timing, the planner setting each point's time from the
  profile; a reversal in a chain stops at the turn; on the wire a
  position's `rate_limit` is its velocity limit. Bench 2026-11-11: moves
  at 10 rad/s track the profile within a few mrad; at the 200 rpm rating
  the voltage ceiling held the rotor 0.5 rad/s short and the clamp's
  back-calculation wound the position integral against the proportional
  term, so the rating is now 18 rad/s and the clamp holds the integral
  instead. The end-to-end path, with the planner examples and the bench
  capture, is written up in `docs/position-trajectory-path.pdf`. The dial under the
  position method writes a position with a 0.01 rad send deadband, so a
  jittering dial does not rewrite a host sequence. Still open: an "after
  the previous move completes" timing alternative, and mixing setpoint
  kinds in one sequence editor.

- **Outer loops:** the field-oriented velocity method `fw~mc_025` — built,
  its Unity and SIL step-response tests pending:
  a PI on ω* − ω̂ in the commutation step sets a torque demand in Nm,
  i_q* = T*/K_t clamped at ±I_max with back-calculation, wrapping the
  torque method (`MODE_FOC_TORQUE`; `MODE_FOC_VELOCITY` is new); K_t joins
  the FOC config; K_vp / K_vi put the loop near 275 rad/s with the
  integral corner at 12.5 rad/s, kept after the gain sweep in
  `tools/trace_analysis/velocity_loop`. Full dial sits at the voltage limit
  under FOC (back-EMF at 200 rpm is ~11.5 V, the 20 V bus's linear ceiling),
  so the acceptance steps to 10 rad/s. Then position control cascaded around
  it, with any velocity-command shaping.
- **Rotor angle and velocity estimation** from the encoder — done: the
  second-order tracking observer `fw~est_rotor_001`/`_002` in state-space
  form, corrected at 1 kHz and carried forward every PWM cycle
  (`app_rotorEstimation`), so FOC commutates on a lag-free angle at
  constant speed. Its constant-velocity model lags under acceleration
  (angle a/K_i, velocity 2ζa/ω_n); specced, next to build: a known-input
  term G·u in the prediction, u = (K_t·I_q − b·ω̂)/J from the measured
  current, G = [T²/2, T]ᵀ, J and b in the estimator config
  (`fw~est_rotor_002` edited in place, `fw~mc_016` sets the input).
- **Motor parameter identification** — R / L / Ke, initially offline
  (measurement + datasheet) to seed the SIL model, with auto-commissioning
  scoped as a later phase.
- Feeds the Kalman-family observer work (README goal 3), developed and
  characterized in SIL before the bench.

## After this sprint (full roadmap, one durable home)

These follow the critical path; (4) can interleave once (2) lands, and (5)'s
protocol decision must precede (6).

4. **Operating modes & device UX** — `sys~ops_001` FSM, knob + mode-button
   semantics, LED-ring feedback, fault presentation. The standalone-first
   device experience.
5. **Observability + persistence** — a real `obs` module (replacing the
   throwaway `telemetryTask` scaffolding), which forces the **deferred CDC
   framing decision** (custom binary vs protobuf-over-stream); and the
   **NVRAM choice** (littlefs vs EEPROM emulation) for calibration/config.
   The protocol is decided here, before the app starts.
6. **Desktop app (Rust)** — live plotting / config / diagnostics against the
   settled protocol. Deliberately last: the device is standalone-first.

## Acceptance (every milestone)

- `tools/oft/oft.sh trace specs/ sw/` has no dangling/broken tags. Uncovered
  specs written ahead of impl are expected; coverage climbs as code lands.
- `tools/validate-specs.py` clean.
- ARM + native build clean; native ctest green.
- Anything that can be closed in SIL is demonstrated in SIL before the bench.

## Known gotchas carried in from the foundation sprint

- **AUTDLY is ADC-wide.** Forced on for the polled regular read, and it stays
  on until that read is DMA-driven: `HAL_ADC_PollForConversion` clears EOC and
  EOS together when AUTDLY is clear, so a multi-rank poll times out on ranks
  2..N. It costs the injected group ~1 trigger in 208 meanwhile
  (`docs/sil/backlog.md`).
- **Encoder RX DMA is parked** — the `HW_SPI` stm32g4 DMA path is TX-only.
  Mirror `HW_SPI_private_dmaTxComplete` for RX if the ~3.5 µs polled read
  becomes a loop-timing problem.
- **Unhandled-IRQ wedge** — any enabled-but-unhandled interrupt hangs the CPU
  in `Default_Handler`. New TIM1/ADC/DMA interrupts need handlers wired
  and reachable through `--whole-archive fw_hw`, or the linker drops them.
