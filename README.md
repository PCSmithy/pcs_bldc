# pcs_bldc

A learning project for modeling and control of a brushless DC motor.

This is a personal reference project. The primary audience is the author, with
the intent to open-source it as a public example of what I believe quality
embedded software development, state estimation, and motor control should look
like end-to-end.

## Status

- **Hardware:** design finalized, layout complete. The board is frozen pending
  firmware work and will only be revised if firmware exposes a project-blocking
  issue.
- **Firmware:** foundation platform complete — a spec'd and traced driver
  stack (clocks, GPIO, ADC, DMA, SPI, timers, USB CDC, op-amps, encoder, LED
  ring, button, serial) with all board sensing bench-verified in engineering
  units and streamed over USB telemetry. Next up is the motor-control sprint
  (see [`docs/motor-sprint.md`](docs/motor-sprint.md)); the gate driver has
  not yet been driven.
- **Simulation and analysis tooling:** not yet started.

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
| `docs/`        | Project-level documentation (spec system, etc.)           |
| `specs/`       | Spec files (created as firmware work begins)              |
| `fw/`          | Firmware source (created as firmware work begins)         |
| `sim/`         | SIL model and benchmark scenarios (TBD)                   |
| `notebooks/`   | Jupyter analysis notebooks (TBD)                          |
| `tools/`       | Desktop visualizer / control app and other tooling (TBD)  |

`CLAUDE.md` contains working notes used by the AI assistant on this project.
