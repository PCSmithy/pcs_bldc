# Motor sprint — drive path, FOC current loop, velocity/position control

The successor to [`docs/foundation-sprint.md`](foundation-sprint.md) (now
complete). The foundation sprint delivered a spec'd + traced driver stack and
bench-verified board sensing; this sprint turns it into a spinning,
closed-loop, field-oriented motor controller. SIL-first: the motor model in
the `sim` target is developed alongside, so controllers and estimators are
validated against the simulation before the bench.

## [START HERE] — entry point

**Where we are:** Milestones 1 and 2 done; Milestone 3 built, with the
position method's bench verification pending. Full hw/io/dev/app driver stack, all 10 analog inputs bench-verified,
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
bootstrap precharge (`fw~mc_021`) are in, as are the three-state rotor
observer `fw~est_rotor_001`/`_002` (angle, velocity, unmodelled torque;
`app_rotorEstimation`, `specs/firmware/est/rotor.md`) that `fw~mc_019`,
the field-oriented methods and the status velocity consume, and the
bridge's zero-current offset tracking (`fw~io_bridge_009`). The
field-oriented torque, velocity and position methods (`fw~mc_022`..`fw~mc_032`,
`specs/firmware/mc/foc.md`, `lib_pi` in `app_motorControl`) are built,
with cogging compensation and the T̂_d feed-forward; their step
responses are tested in Unity and in SIL (`tests/foc.rs`,
`tests/velocity_method.rs`, `tests/position_method.rs`). The host command
plane drives them. Deferred: the V/f method `fw~mc_010`/`fw~mc_017`
(`specs/firmware/mc/vf-sinusoidal.md`) is the sensorless start-up ramp,
scheduled with sensorless FOC.

**Read first:**
1. `CLAUDE.md`, plus the ADC and unhandled-IRQ gotchas at the bottom of this
   file.
2. `specs/system/mc/motor-control.md` (`sys~mc_001`, the sprint's system
   anchor) + `specs/firmware/hal/adc.md` (`fw~hal_adc_003` / `fw~hal_adc_008`,
   the timer-triggered injected conversions and their async completion).
3. `specs/firmware/hal/tim.md` (`HW_TIM`) — the base that Milestone 1 extends
   for complementary PWM.

**Workflow** is unchanged from the foundation sprint: use the `pcs_spec`
skill to author/extend specs (interview → two research agents → draft →
blind style audit → write + tag + test), keep native ctest and the ARM link
green, and hold OFT at no dangling tags. New parents live under `sys~mc_001`.

## Milestones (critical path: 1 → 2 → 3)

### 1. Drive-path bring-up

First motion, open-loop, at low voltage/current. Proves the gate-driver
control path and the protective plumbing before any closed loop.

- **`HW_TIM` extension** — done: TIM1 3-phase complementary PWM
  (CH1/2/3 + CH1N/2N/3N), center-aligned with dead-time, its update event
  on the crest (`specs/firmware/hal/tim.md`).
- **STSPIN32G4 configuration + fault handling** — done (`dev_gateDriver`,
  `specs/firmware/mc/gate-driver.md`); a hardware break (nFAULT/BKIN)
  latches a fault.
- **Open-loop commutation** — done: six-step (`fw~mc_011`/`fw~mc_012`).
- **Sensored sinusoidal commutation** — done (`fw~mc_020`, voltage-mode on
  the per-cycle rotor angle `fw~mc_019`): the SVM path the d/q frame FOC
  closes its loops in.
- **Software overcurrent trip** — done: 1.2 A phase / 1.5 A bus trips off
  the INA240 sense channels disable the bridge. A latched fault leaves the
  drive disabled after a clear.

### 2. FOC current loop

Sensored inner torque loop, SIL-first, using the U/V ADC-instance split
already in place.

- **Timer-triggered injected sampling** — done: TIM1-triggered injected
  conversions on ADC1 (U) and ADC2 (V), 4× hardware oversampled, with the
  per-cycle callback (`fw~hal_adc_010`, `fw~io_bridge_006`/`_007`).
- **Zero-current offset tracking** — done (`fw~io_bridge_009`): the bridge's
  EMA of each idle phase's reading, latched when the phase next drives.
- **Clarke / Park transforms, SVM** — done (`fw~mc_013`/`_014`,
  `fw~io_bridge_008`).
- **PI current loops (Id/Iq)** — done (`fw~mc_022`..`fw~mc_024`):
  `lib_pi` regulators with K_p = L·ω_bw / K_i = R·ω_bw, vector limit at
  min(V_max, V_bus/√3) with back-calculation anti-windup, i_q* = T_q/K_t
  from the torque target in Nm (`fw~mc_026`). Board config: R = 14.7 Ω,
  L = 5 mH, K_t = 0.825 Nm/A, ω_bw = 1000 rad/s, I_max = 1 A under the
  1.2 A phase / 1.5 A bus trips. The step response and the limit hold on
  the SIL's held rotor (`tests/foc.rs`).
- **Encoder electrical-angle alignment** — done (`fw~mc_012`).
- **Control task** driven by the conversion-complete ISR — done
  (`fw~mc_015`).
- **SIL:** motor electrical model with a rotor lock (`MotorParams::rotor_locked`);
  the FOC step responses run on the 50 µs grid against it. No SIL test
  carries `[test->sys~mc_001~1]` yet; a closed-loop tracking run is the
  natural first.

### 3. Velocity / position control + estimation

Outer loops and the first estimator work.

- **Host command plane** — done: one `Command` carrying a mode and a timed
  setpoint list that `app_sequencer` plays on the 1 ms tick
  (`fw~conn_server_007`, `fw~ops_001`/`_002`), torque, speed and position
  setpoint kinds with a per-setpoint rate limit (`fw~mc_026`, `fw~mc_027`,
  `fw~mc_030`), the motor rating report `MotorInfo` (`fw~conn_server_008`),
  and the app's Command and Sequence widgets with a capture export
  (`app~conn_004`, `app~views_024`..`_028`). A stop setpoint ends its
  sequence; a setpoint kind the method cannot use is rejected; a USB
  detach leaves a sequence playing; the first button tap stops a
  host-started motor. A new setpoint kind is one `Setpoint` oneof arm, one
  per-kind write in the sequencer, one target struct in motor control, one
  bound in `MotorInfo` and one unit/bound row in the widgets.
- **Velocity method** `fw~mc_025` — done: a PI on ω* − ω̂ sets a torque
  demand in Nm, i_q* = T*/K_t, clamped at ±I_max with back-calculation
  (`MODE_FOC_VELOCITY`). K_vp = 0.04, K_vi = 0.5 put the loop near
  275 rad/s at J = 1.45e-4 with the integral corner at 12.5 rad/s, kept
  after the gain sweep in `tools/trace_analysis/velocity_loop`. The
  integral holds under the voltage limit. Full speed sits at the voltage
  ceiling (back-EMF ≈ 11.5 V at 200 rpm on the 20 V bus), so the velocity
  rating is 18 rad/s.
- **T̂_d feed-forward** (`fw~mc_025`, k_ff = 1): the observer's
  unmodelled-torque estimate is subtracted from the velocity demand, so
  friction and load are cancelled before the integrator winds up; it
  removes the stick-slip at crawl speeds.
- **Cogging compensation** `fw~mc_028` — done: two harmonics of the
  cogging torque on the observer's mechanical angle (28 per turn at
  9 mNm, 56 per turn at 0.9 mNm, read from the field-oriented hold's T̂_d)
  are subtracted from every field-oriented demand; the compensated share
  T_k joins the observer's torque input (`fw~mc_016`), so T̂_d reads the
  residual rather than cancelling the ripple a second time.
- **Position method** `fw~mc_029` — built, bench verification pending: a
  PI on the rotor position `fw~mc_032` with the move profile's velocity
  fed forward, cascaded around the velocity method; the position setpoint
  `fw~mc_030` and the on-board move profile `fw~mc_031` (a minimum-time
  trapezoid under per-setpoint velocity and acceleration limits, a
  velocity at the point for chained moves, a jog from the speed target),
  its position target interpolated in the 20 kHz loop; Telemetry carries
  the position (`fw~obs_status_001`). A multi-turn position, zero at the
  bridge enable, bounded to ±10 turns; the sequence chart draws the same
  profile the board plans; a reversal in a chain stops at the turn; on
  the wire a position's `rate_limit` is its velocity limit. The integral
  holds while the velocity command is clamped. SIL: a 2π move lands
  within 1 mrad, a 10 mNm load at rest moves the rotor 2 mrad. The dial
  under the position method writes a position with a 0.01 rad send
  deadband. The end-to-end path is written up in
  `docs/position-trajectory-path.pdf`. Open: an "after the previous move
  completes" timing alternative, and mixing setpoint kinds in one
  sequence editor.
- **Rotor angle and velocity estimation** — done: the three-state observer
  `fw~est_rotor_001`/`_002` on angle, velocity and unmodelled torque T̂_d,
  corrected at 1 kHz and carried forward every PWM cycle on the known
  input u = (K_t·I_q + T_k − b·ω̂ − T_c·sat(ω̂/ω_db) + T̂_d)/J
  (`app_rotorEstimation`). J = 1.45e-4 kg·m², b = 1e-4, T_c = 15 mNm
  from the bench. The order decision and its captures are in
  `tools/trace_analysis/estimator_order`; the derivation is
  `docs/references/rotor-observer-derivation.md`.
- **Motor parameter identification** — R / L / Ke, initially offline
  (measurement + datasheet) to seed the SIL model, with auto-commissioning
  scoped as a later phase.
- Feeds the Kalman-family observer work (README goal 3), developed and
  characterized in SIL before the bench.

## Open design items

The control design is the owner's; these are the known gaps in it.

- **Discrete pole placement for the observer.** The gains are placed in
  continuous time and applied once per 20 prediction steps, so the
  realised discrete poles at the board config are about z = 0.290 and
  0.86 ± 0.047j (≈ −1240 rad/s and −149 ± 55j rad/s) against the designed
  −314, −314, −157 rad/s (`docs/references/rotor-observer-derivation.md`,
  section 6).
- **Encoder-latency compensation.** θm is compared against a θ̂ read later
  in the 1 ms pass, a lag of ω·d: about 50 mrad electrical at 18 rad/s for
  d ≈ 0.2 ms.
- **float32 θ* resolution.** Past 32 rad one float step is 3.8e-6 rad, so
  a jog under about 1.9 mrad/s stalls.
- **Position bench verification** of `fw~mc_029`..`fw~mc_032` is pending.

## After this sprint (full roadmap, one durable home)

4. **Operating modes & device UX** — `sys~ops_001` FSM, knob + mode-button
   semantics, LED-ring feedback, fault presentation. The standalone-first
   device experience.
5. **Persistence** — the **NVRAM choice** (littlefs vs EEPROM emulation)
   for calibration and config.
6. **Desktop app** — configuration and diagnostics on top of the
   observability, command and sequence slice. The device stays
   standalone-first.

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
