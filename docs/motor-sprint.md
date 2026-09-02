# Motor sprint — drive path, FOC current loop, velocity/position control

The successor to [`docs/foundation-sprint.md`](foundation-sprint.md) (now
complete). The foundation sprint delivered a spec'd + traced driver stack and
bench-verified board sensing; this sprint turns it into a spinning,
closed-loop, field-oriented motor controller. SIL-first: the motor model in
the `sim` target is developed alongside, so controllers and estimators are
validated against the simulation before the bench.

## [START HERE] — entry point

**Where we are:** foundation sprint done (M1–M5). Full hw/io/dev/app driver
stack, all 10 analog inputs bench-verified in engineering units, FreeRTOS
task architecture (`task_1ms` > `task_10ms` > `task_usb` > `telemetryTask`)
running. The gate driver has **never been driven** — no motor motion yet.

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
- **Open-loop commutation.** V/f (scalar) or six-step first spins at reduced
  bus voltage / current-limited, to confirm phase ordering, sense polarity,
  and mechanical rotation.
- **Software overcurrent trip** off the already-verified phase-current sense
  channels (INA240 phase I, VBUS I): a fast poll/ISR threshold that disables
  the bridge. This is the standing safety net for everything after.

### 2. FOC current loop

Sensored inner torque loop, SIL-first, using the U/V ADC-instance split
already in place.

- **Timer-triggered injected sampling** — retire the reserved
  `fw~hal_adc_003` / `fw~hal_adc_008`: TIM1-update-triggered injected
  conversions on ADC1 (U) and ADC2 (V) for simultaneous phase sampling,
  interrupt- or DMA-readout, NVIC priorities that keep USB from jittering the
  loop. **The regular path must move to DMA first** — AUTDLY is ADC-wide and
  is forced on for the polled read, which drops injected triggers that arrive
  while it holds the sequencer (`docs/sil/backlog.md`).
- **Zero-current offset calibration** at startup (bridge disabled).
- **Clarke / Park transforms, PI current loops (Id/Iq), SVM** output back to
  the TIM1 duty registers.
- **Encoder electrical-angle alignment** — align AS5048 mechanical angle to
  the electrical frame (pole-pair count, offset find).
- **Control task** driven by the hardware timer / conversion-complete ISR,
  not the 1 ms poll.
- **SIL:** motor electrical model (R/L/Ke, bridge, sensor models) in the
  `sim` target so the current loop is closed in simulation first.

### 3. Velocity / position control + estimation

Outer loops and the first estimator work.

- **Outer loops:** velocity PI and position control cascaded around the
  current loop.
- **Velocity estimation** from encoder angle; first `est`-topic specs beyond
  the encoder driver.
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
