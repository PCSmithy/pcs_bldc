---
status: draft
tags: [system, architecture, overview]
---

# System overview

This document describes the `pcs_bldc` system at the architectural level: what
it is, what its major pieces are, how they cooperate, and the small set of
foundational `sys~` specifications that anchor those choices. Topic-specific
behavioral specs (motor control, estimation, observability, etc.) live in
sibling files under `specs/system/<topic>/`.

## Purpose

`pcs_bldc` is a USB-PD-powered, single-axis BLDC motor controller used as a
benchtop development platform for learning state estimation and motor
control. It is a **standalone, fully-operational device** with on-device
controls, augmented (but not gated) by an optional desktop application that
provides observability, configuration, diagnostics, and scripted operation.

## Block diagram

```text
  ┌─────────────────────┐
  │   Desktop app       │      USB CDC (virtual COM)
  │   (Rust, optional)  │─────schema-defined protocol─────┐
  │                     │                                 │
  │   - Live plotting   │                                 │
  │   - Configuration   │                                 │
  │   - Diagnostics     │                                 ▼
  │   - Scripted runs   │                  ┌──────────────────────────┐
  │   - Multi-device    │                  │   pcs_bldc device        │
  └─────────────────────┘                  │                          │
                                           │   STM32G431 firmware     │
   On-device UI ─────────────────────────► │   (C/C++ on FreeRTOS)    │
   - Control knob + encoder                │                          │
   - Mode-select button                    │   - FOC control          │
   - RGB status LEDs                       │   - Kalman estimator     │
                                           │   - Mode FSM             │
   AS5048 magnetic encoder ──────────────► │   - On-device UI logic   │
   (rotor position)                        │   - NVRAM (flash-based)  │
                                           │   - Telemetry stream     │
                                           │                          │
                                           │   STSPIN32G4 gate driver │
                                           │   + 3-phase bridge       │
                                           └────────────┬─────────────┘
                                                        │ 3-phase
                                                        ▼
                                                ┌────────────────┐
                                                │   BLDC motor   │
                                                │   (unloaded)   │
                                                └────────────────┘

  Power input: USB-PD (CYPD3177 sink controller) → 5V buck (LMR50410)
```

## The two software components

| Component         | Language     | Hosted on             | Required? |
|-------------------|--------------|-----------------------|-----------|
| Firmware          | C / C++      | STM32G431 (FreeRTOS)  | Always    |
| Desktop app       | Rust         | Windows / macOS host  | Optional  |

The firmware is **primary**: it owns the device's behavior, the operating-mode
state machine, on-device UI, control loops, estimator, and persistence. The
desktop app is **auxiliary**: it adds capabilities that benefit from a
keyboard, screen, and disk — observability plots, parameter configuration,
diagnostic queries, scripted/preset operation, and (stretch goal) coordinated
multi-device control. The device must be fully usable with the app
disconnected.

## External interfaces (system boundary)

- **USB 1** — power input via USB-PD over USB-C cable. Negotiated
  by the CYPD3177; stepped to 5V by the LMR50410 buck.
- **USB 2** - data over USB-C cable between the device and PC for data exchange between the motor driver device and the desktop app
- **Three-phase motor output** — driven by the STSPIN32G4-integrated bridge,
  current-sensed via per-phase shunts.
- **AS5048 magnetic rotor encoder** — over SPI, providing 14-bit absolute
  rotor position for sensored operation.
- **On-device user controls** — a control knob with encoder, a
  push-button for mode selection, and several RGB status LEDs. These provide
  the standalone-mode user experience.

## Internal interface — firmware ↔ desktop app

- Transport: **USB CDC** (virtual COM port). No special drivers on either
  Windows or macOS.
- Direction: **bidirectional**. Commands flow down (mode changes, setpoints,
  parameter writes); telemetry flows up (control loop internals, references,
  estimator state, raw sensor data).
- Protocol: schema-defined protocol-buffer messages in CRC-validated
  frames — `sys~conn_001~1` (schema), `sys~conn_002~1` (framing),
  `sys~conn_003~1` (request acknowledgement), under `specs/system/conn/`.
- Disconnection: the firmware must continue executing whatever mode it was
  last commanded to when the host disconnects mid-session; the app must
  handle disconnect gracefully and reconnect when the device returns.

## Operating modes (high-level state machine)

```text
                     ┌─────────────┐
                     │   Idle      │  Bridge disabled, accepting commands
                     │             │  Default state at boot
                     └──┬────┬─────┘
                        │    │
              ┌─────────┘    └─────────┐
              ▼                        ▼
       ┌────────────┐            ┌────────────┐
       │ Calibrate  │            │   Run      │
       │            │            │            │  Sub-modes:
       │ Encoder    │            │ Closed-loop│   - Torque
       │ offset,    │            │ control    │   - Velocity
       │ param ID   │            │ active     │   - Position
       └─────┬──────┘            └──────┬─────┘   - Trajectory
             │                          │
             └────────────┬─────────────┘
                          │
                          ▼
                   ┌─────────────┐
                   │   Fault     │  Protection tripped, bridge disabled
                   │             │  Requires explicit clear (button or app)
                   └─────────────┘
```

Mode transitions can be triggered by either source:

- The **on-device button** (and knob, for setpoint changes within a mode).
- The **desktop app** (when connected).

Both inputs flow into the same mode-management logic; the firmware does not
distinguish source. Faults always disable the bridge and require an explicit
clear.

## Persistence

Settings that must survive power cycles are persisted in the STM32G431's
internal flash via an NVRAM library (no external EEPROM is fitted). The
implementation choice is between **littlefs** (general-purpose embedded
filesystem) and an **emulated-EEPROM middleware** (ST's X-CUBE-EEPROM or
similar). Decision is deferred and will live in a `fw~` spec under
`specs/firmware/persistence/`.

Persistent items (initial set):

- Motor parameters: R, L, Kt, J, B.
- Encoder calibration: zero offset, direction, electrical-pole-pair count.
- User preferences: any on-device UI defaults, last-active mode/setpoint.

The desktop app is *not* the source of truth for any of these — it can read
and write them, but the firmware owns the canonical values.

## Plurality

- A single physical device serves a single user at any moment via its
  on-device controls.
- The desktop app shall be capable of discovering and connecting to multiple
  devices simultaneously, presenting each with its own UI panel.
- **Stretch:** coordinated position-trajectory control across multiple
  devices (e.g. a master trajectory replayed in lockstep across N devices
  for comparative experiments).

## Constraints and non-goals

(Repeated here for context; see `README.md` for the canonical project-goal
list.)

- The motor is unloaded; no production load profile is in scope.
- HIL is not a project goal; SIL is the simulation surface.
- Basic safety only — overcurrent, overtemp, encoder-loss, gate-driver
  fault. No certification target.
- Hardware is frozen unless firmware work uncovers a project-blocking issue.

## Roadmap milestones

For navigation only — milestones do not have spec IDs.

- **MVP 1.** Motor spins under commanded constant velocity, with on-device
  setpoint via the knob; live measured-velocity plot streamed to the
  desktop app over USB CDC. Single hard-coded set of motor parameters.
- **MVP 2.** Position state estimation (Kalman family) and closed-loop
  position control, with the same standalone-plus-app pattern.
- **Beyond.** Auto-commissioning of motor parameters; sensorless estimation
  experiments; sensor-degradation studies; stretch coordinated multi-device
  control.

## Anchor specifications

These are the small set of cross-cutting `sys~` requirements that anchor the
architecture. Topic-specific behavioral specs (e.g. for FOC, estimator,
trajectory tracking, telemetry framing) live under `specs/system/<topic>/`
and trace to whichever of these anchors are relevant.

### Device standalone operation
`sys~arch_001~1`

The device shall be fully operational with no host computer or desktop
application connected. All primary operations — mode selection (Idle,
Calibrate, Run-with-sub-mode), setpoint adjustment, fault clearing, and
status reporting — shall be available through the on-device controls
(button, knob, RGB LEDs).

Acceptance:

- With USB connected for power only (no host enumerating the data
  interface), the device boots into Idle, allows the user to enter
  Calibrate and Run modes via the button, allows in-mode setpoint
  adjustment via the knob, and reports current mode and fault state via
  the RGB LEDs.
- A bench scenario exercising every supported mode end-to-end completes
  successfully without any host-side software.

Covers:

- (project goal: README.md, "Reference-quality embedded software
  development" — the firmware owns the device experience and is not
  dependent on auto-generated framework code or external orchestration.)

Needs: fw, test

### Desktop app as auxiliary capability
`sys~arch_002~1`

The desktop application shall provide observability, control,
configuration, diagnostics, and scripted-operation capabilities that
augment but do not replace the device's standalone functionality. The
app is optional; its absence shall not impair any standalone device
behavior.

Acceptance:

- The app exposes UIs for: live telemetry plotting, device control (mode
  selection, setpoint adjustment, and fault clearing per `sys~ops_001~1`),
  motor-parameter and user-preference configuration, diagnostic queries
  (firmware version, fault history, current mode), and scripted operation
  (preset trajectories).
- Disconnecting the app at any time during device operation does not
  affect the firmware's ongoing mode or control behavior.
- Each detailed app capability is covered by its own `app~` spec under
  `specs/desktop-app/`.

Covers:

- (project goal: README.md, "Modeling, simulation, and observability
  infrastructure" — the desktop visualizer / control app.)

Needs: app, test

### USB CDC transport
`sys~arch_003~1`

The firmware and desktop app shall communicate over a USB CDC (virtual
serial port) interface carrying the protocol defined in `sys~conn_001~1`.

Acceptance:

- The device enumerates on Windows and macOS using the OS in-box CDC
  class driver.
- A round-trip request/response exchange succeeds over the open port.

Covers:

- (project goal: README.md, "Modeling, simulation, and observability
  infrastructure" — telemetry streaming + control surface.)

Needs: fw, app, test

### Operating mode state machine
`sys~ops_001~1`

The firmware shall implement a mode state machine with the top-level
states **Idle**, **Calibrate**, **Run**, and **Fault**. Run shall have
sub-modes for **Torque**, **Velocity**, **Position**, and **Trajectory**
control. Mode transitions shall be triggerable from either the on-device
button or a desktop-app command and shall be processed identically
regardless of source. Fault shall always disable the bridge and shall
require an explicit clear command (button long-press or app command).

Acceptance:

- Every documented transition is exercisable in SIL and on the bench.
- An Idle → Run transition with no Calibrate run since boot is rejected
  if calibration data is absent (encoder offset unset).
- A fault entered from any Run sub-mode disables the bridge within the
  fault response time budget (TBD in safety specs).
- The mode-management code path is identical whether the trigger source
  is the button or the app.

Covers:

- (project goal: README.md, "Reference-quality embedded software
  development" — clean abstraction and channelized design.)

Needs: fw, test

### Persistence in flash NVRAM
`sys~persist_001~1`

The firmware shall persist motor parameters, encoder calibration data,
and user preferences in the STM32G431's internal flash via a
flash-based NVRAM mechanism (specific implementation chosen in a `fw~`
spec). Persisted values shall survive power cycles and shall be the
firmware's source of truth — the desktop app may read and write these
values but does not own them.

Acceptance:

- A round-trip write (via app or on-device UI) followed by power cycle
  followed by read returns the written value.
- A power loss during write does not corrupt previously-persisted
  values.
- On a fresh device with no prior writes, reads return defined defaults.

Covers:

- (project goal: README.md, "Reference-quality embedded software
  development" — persistent device state, owned by firmware.)

Needs: fw, test

### Multi-device support in the desktop app (stretch)
`sys~arch_004~1`

The desktop application shall be capable of discovering, connecting to,
and operating multiple `pcs_bldc` devices simultaneously. Each device
shall be presented as its own UI panel; commands and telemetry shall be
fully isolated between devices.

Acceptance:

- With N ≥ 2 devices physically connected, the app discovers and lists
  all of them.
- Issuing a command to one device does not affect the others.
- Telemetry streams from all connected devices are decoded and rendered
  concurrently without dropped frames at the working data rate.

Covers:

- (project goal: README.md, "Field-oriented motor control" — repeatable
  trajectory benchmark; multi-device coordination is a stretch goal that
  enables comparative experiments across identical hardware.)

Needs: app, test

### Hardware abstraction layer for representative simulation
`sys~arch_005~1`

The firmware shall isolate all MCU-peripheral access behind a hardware-
abstraction (HW) layer whose modules present an identical, target-independent
API across the embedded (STM32G4) and simulation (SIM) build targets, so that
IO-, device-, and application-layer code can be exercised unchanged in a
high-fidelity SIL environment. Each HW module shall be self-contained and
reusable across projects without modification, with only its channel/bus
configuration and target-specific implementation supplied per project.

Acceptance:

- Every HW-layer module compiles and links for both STM32G4 and SIM behind
  a single consumer-facing header API.
- IO/dev/app code built against the HW API requires no source changes to
  move between targets; only the channel/bus configuration and the
  target-specific HW implementation differ.
- A HW module can be dropped into a separate project and built for both
  targets with only its channel/bus configuration supplied by the new
  project.

Covers:

- (project goal: README.md, "Reference-quality embedded software
  development" + "Modeling, simulation, and observability infrastructure" —
  SIL is the primary validation surface, which requires well-contained,
  reusable hardware abstractions with target-swappable implementations.)

Needs: fw, test
