# pcs_bldc

A learning project for modeling and control of a brushless DC motor.

This is a personal reference project. The primary audience is the author, with
the intent to open-source it as a public example of what I believe quality
embedded software development, state estimation, and motor control should look
like end-to-end.

## Status

*As of 2026-11-27.* The board drives the motor under field-oriented torque,
velocity and position control, commanded from its own knob or from the
desktop app, and every piece of that is specified, traced and tested in
simulation. The estimation goals beyond a fixed-gain observer, sensorless
operation, and the comparison harness are not started.

- **Hardware:** one board revision, built and frozen. It is revised only if
  firmware exposes a blocking issue. One assembly substitution (a 24 MHz
  crystal for the 25 MHz part) is logged in
  [`hw/rework-log.md`](hw/rework-log.md) and not yet in the schematic.
- **Firmware:** a spec'd and traced driver stack (clocks, GPIO, ADC, DMA,
  SPI, I2C, timers, USB CDC, op-amps, CORDIC, encoder, LED ring, button)
  with all board sensing bench-verified in engineering units. Motor drive
  is six-step, sensored sinusoidal, and field-oriented torque, velocity
  and position, with a move profile, cogging compensation and a
  disturbance-torque feed-forward, on a three-state encoder observer
  (angle, velocity, unmodelled torque). Protection is a phase and bus
  overcurrent trip, an encoder-fault trip and a gate-driver break latch.
  A protobuf-over-USB protocol carries telemetry, a signal trace sampled
  at the 20 kHz PWM rate, a log stream, and a command plane that plays
  timed setpoint sequences on the board. USB-PD sink monitoring reports
  the negotiated supply. At the 170 MHz core clock the 20 kHz control
  interrupt averages 23 µs of its 50 µs period at full speed with a
  32-signal trace, and its timing requirement passes on the bench
  ([`docs/bench/cycle-timing.md`](docs/bench/cycle-timing.md)).
- **Desktop app:** connects behind a firmware-identity gate, picks signals
  from the firmware's DWARF, and plots them live (WebGL, min/max
  decimation) with a pause/zoom timeline, comparison cursor, value table
  and session restore. Command and Sequence widgets drive the command
  plane, and a capture exports to CSV. Spec-driven
  ([`specs/desktop-app/`](specs/desktop-app/)) and verified by a
  playwright suite.
- **Simulation:** the SIL harness (`sw/sil`) runs the real firmware, built
  natively with the same RTOS tasks, against a motor, bridge and sensor
  model at the bench-fitted parameters, with cogging and load torque. It
  is the main verification surface: 304 tests, alongside 44 unit-test
  suites.
- **Traceability:** 233 requirements. Every firmware and app requirement
  that is built traces to code and to a test; requirements only the
  hardware can show are checked on the bench by scripts that write a
  dated record ([`docs/bench/`](docs/bench/)). CI builds the firmware and runs the unit and SIL suites on
  Windows, macOS and Linux, and the app suites on Windows and macOS.

### Against the goals

| Goal | In place | Not started |
|------|----------|-------------|
| 1. Embedded software | Layered, channelized firmware on FreeRTOS; spec system with OpenFastTrace; unit + SIL tests; three-OS CI | Static analysis, a pinned containerized toolchain, and a spec-trace gate in CI |
| 2. Modeling and observability | SIL plant fitted to the bench motor; 20 kHz trace and telemetry; desktop app; offline trace analyses in `tools/trace_analysis/` | The `notebooks/` pipeline; the algorithm comparison harness |
| 3. State estimation | Fixed-gain three-state observer on the encoder, characterized in SIL and on the bench | Kalman-family observer; thermal state; sensor-degradation experiments; sensorless observer; auto-commissioning |
| 4. Motor control | FOC torque loop; velocity, torque and position loops; position moves with a trapezoidal profile | Sensorless inner loop (the V/f start-up ramp is specified, not built); the repeatable trajectory benchmark |

Also specified at system level and not yet built: the operating-mode state
machine, settings persisted in flash, and multi-device support in the app.
Open work is tracked in [`docs/backlog.md`](docs/backlog.md),
[`docs/sil/backlog.md`](docs/sil/backlog.md) and
[`docs/motor-sprint.md`](docs/motor-sprint.md).

### How it got here

| When | Milestone |
|------|-----------|
| 2025-10 to 2026-03 | Schematic and layout; design checked and frozen |
| 2026-04 to 2026-06 | Spec system and build infrastructure; the channelized driver stack, peripheral by peripheral |
| 2026-07 | SIL harness running the real firmware natively on three operating systems, with CI; first protected six-step drive |
| 2026-08 | Full-loop commutation in SIL on a bench-parameterized plant; USB protocol and the desktop app's observability slice |
| 2026-09 | Timer-triggered current sampling at the PWM crest; ADC over DMA; the 20 kHz signal trace |
| 2026-10 | C++ for the control modules; space-vector modulation; sensored sinusoidal drive; the interrupt hot path; a rotor tracking estimator |
| 2026-11 | Field-oriented torque, velocity and position control; the host command plane; the three-state observer; the control interrupt inside its timing budget (link-time optimization, 170 MHz core clock) |

Decisions made along the way:

- **C++ for control code.** The firmware started as C; the bridge, motor
  control, estimator and sequencer are now C++ classes behind C facades,
  and new control code starts in C++
  ([`docs/cpp-coding-conventions.md`](docs/cpp-coding-conventions.md)).
- **A Tauri desktop app.** The app, planned only as "Rust", is a Rust
  core with a static web frontend and no frontend build step.
- **Protobuf on the wire.** An early text telemetry stream gave way to
  COBS-framed protobuf with a board-specific schema.
- **A deterministic observer first.** The estimator in use is a
  fixed-gain Luenberger observer; the Kalman-family work the goals call
  for comes after it.
- **Linux in CI.** The supported development platforms are Windows and
  macOS; the firmware and SIL also build and test on Linux.

The hardware is a USB-PD-powered BLDC controller built around an STM32G431 MCU,
an STSPIN32G4 integrated gate driver, and an AS5048 magnetic rotor encoder. It
will live on the desk as an unloaded test stand; no production load profile is
in scope.

## Setup

Run `./setup.sh` from the repo root. It checks for required system tools
(Java 17+, Python 3.10+; KiCad 10 if you are doing hardware work), prints
install instructions for anything missing, and then installs project-local
artifacts (the OpenFastTrace JAR and the Python virtual environment). It is
idempotent and safe to re-run.

KiCad is required **only** for hardware design work. Contributors working on
firmware, SIL, analysis notebooks, or desktop tooling can ignore a
missing-KiCad warning and proceed.

Supported platforms are Windows and macOS. See
[`docs/setup.md`](docs/setup.md) for the full prerequisite table, what the
script does, verification steps, and troubleshooting.

## Project Goals

The project has four primary goals. Each is treated as a first-class
deliverable with its own specs, tests, and documentation. The goals are not
ordered by priority; they are interdependent and developed together.

### 1. Reference-quality embedded software development

Demonstrate, as a working example, what disciplined embedded software
development looks like end-to-end.

- **Architecture.** FreeRTOS-based, with clean abstraction layers, channelized
  module design for reuse, and no reliance on auto-generated framework code as
  the source of truth for application logic.
- **Spec-driven development with full traceability.** Every line of application
  code links to a software requirement; every requirement links forward to at
  least one test that covers it; every requirement derives from a higher-level
  parent (ultimately a project-goal section in this document). See
  [`docs/spec-system.md`](docs/spec-system.md).
- **Test infrastructure.** Unit tests for module-level logic; high-fidelity
  software-in-the-loop (SIL) tests as the primary verification surface. No
  hardware-in-the-loop (HIL) layer — once the motor is characterized, the SIL
  model is the test bench. Coverage targets are *spec coverage*, not just line
  coverage.
- **CI and quality gates.** Reproducible builds (pinned toolchain in a
  containerized dev environment), static analysis (clang-tidy / cppcheck),
  unit + SIL tests, and spec-traceability checks all enforced on every push.
- **Documentation as deliverable.** Code, specs, and analysis notebooks are
  written for an external reader from day one. The repository is intended to
  be public-ready throughout its life.

### 2. Modeling, simulation, and observability infrastructure

The infrastructure that supports learning and verification is itself a primary
deliverable, not an afterthought.

- **High-fidelity SIL model.** Motor electrical and mechanical model, gate
  driver and bridge model, sensor models (encoder, current shunts, bus
  voltage). Fidelity is set by what the estimators and controllers need to be
  developed and validated against — not by an abstract realism target.
- **System identification feeds the model.** Initial parameters are taken from
  measurements and datasheets to get the motor spinning. Auto-commissioning
  routines and online (augmented-state) parameter identification are scoped as
  later phases.
- **Real-time observability.** Rich USB-streamed telemetry (control loop
  internals, references, estimator states, raw sensor data) at meaningful
  rates, paired with a custom desktop visualizer / control app for live
  inspection, command, and tuning.
- **Offline analysis.** A Jupyter-notebook pipeline for post-hoc analysis with
  derivations, math overlays, and side-by-side comparisons. The notebooks
  carry the academic content of the project and are the primary artifact for
  re-learning estimation and control concepts at depth.
- **Algorithm comparison harness.** Code, SIL scenarios, and analysis pipeline
  structured so that swapping in a new estimator or controller, running the
  standard benchmark trajectories, and producing comparison plots is a small
  and repeatable workflow. The harness is designed to scale across many
  implementations even though the project may only execute on two or three of
  each.

### 3. State estimation

Re-learn and demonstrate optimal-estimation techniques on a real, instrumented
system. Pedagogical clarity and a strong intuition for filter behavior matter
as much as the filter "working."

- **Kalman-family observer** estimating motor position, velocity, acceleration,
  torque, and thermal state from phase currents, phase voltages, and rotor
  encoder position.
- **Validation in SIL before bench.** The SIL/observer pair is used to
  characterize convergence, noise sensitivity, and steady-state error before
  the filter ever sees the real motor.
- **Sensor-degradation experiments.** Systematically drop, delay, or corrupt
  sensor inputs in SIL to characterize filter robustness and to assess the
  feasibility of sensorless operation.
- **Sensorless extension.** Demonstrate sensorless estimation of position and
  velocity using an appropriate observer family (back-EMF, sliding-mode, HFI,
  or similar — selected during design).
- **Future phases.** Auto-commissioning of motor parameters; online
  augmented-state parameter identification.

### 4. Field-oriented motor control

Drive the motor cleanly under a hierarchy of control modes, using estimator
output as the feedback source so that estimation quality directly translates
into control quality.

- **Inner FOC torque loop**, both sensored and sensorless.
- **Outer loops:** constant velocity, constant torque, position control, and
  position trajectory tracking.
- **Repeatable trajectory benchmark.** A canonical position trajectory that
  can be replayed across estimator and controller variants under identical
  conditions, producing standard performance metrics (tracking error, control
  effort, settling time, current ripple, etc.) for side-by-side comparison.
  This benchmark is the primary scenario through which estimator and
  controller variants are evaluated.

## Non-goals

Explicitly out of scope, to keep the project tractable:

- Hardware-in-the-loop infrastructure. SIL is the simulation surface; the
  bench is everything else.
- Production-grade safety certification (DO-178, ISO 26262, or similar).
  Basic protections — overcurrent, overtemp, encoder-loss, gate-driver fault
  handling — are sufficient.
- Loaded operation, multi-motor systems, or specific application profiles
  (servo arm, drone, etc.). The motor is an unloaded desk test stand.
- Hardware revisions, unless firmware work exposes a project-blocking issue
  that cannot be worked around in software.
- Beating commercial motor-control SDKs on raw performance. Comparison against
  a baseline (e.g. ST Motor Control SDK) is interesting if it serves the
  learning goal, but is not a target in itself.

## Repository layout

| Path           | Contents                                                  |
|----------------|-----------------------------------------------------------|
| `hw/`          | KiCad 10 schematic and PCB design                         |
| `datasheets/`  | Reference PDFs for major ICs                              |
| `docs/`        | Project-level documentation (spec system, setup, backlog) |
| `specs/`       | OFT spec tree: `sys~` / `fw~` / `app~` requirements       |
| `sw/`          | All software: `fw/` firmware, `gui/` desktop app, `sil/` SIL harness, `lib/` shared C + Rust libraries, `proto/` board schema, `cmake/` toolchains |
| `tools/`       | Project tooling: OFT wrapper, spec scripts, build scripts |
| `notebooks/`   | Jupyter analysis notebooks (planned)                      |

`CLAUDE.md` contains working notes used by the AI assistant on this project.

## License

- Firmware, tooling, scripts, specs, and docs: **Apache-2.0** ([`LICENSE`](LICENSE)).
- Hardware design in [`hw/`](hw/): **CERN-OHL-W-2.0** ([`hw/LICENSE`](hw/LICENSE)).
- Vendored third-party code keeps its own license.

See [`LICENSES.md`](LICENSES.md) for the full map, including third-party components.
