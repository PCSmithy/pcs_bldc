# System specs

`sys~` requirements: what the system as a whole must do, including how the
STM32G4 firmware and the Rust desktop app cooperate.

Each `sys~` spec declares which downstream components it requires for
coverage on a per-spec basis:

- `Needs: fw, test` — firmware-only behavior (e.g. closed-loop current
  control runs at 20 kHz on the MCU).
- `Needs: app, test` — desktop-app-only behavior (e.g. the user can change
  the live-plot color scheme).
- `Needs: fw, app, test` — cross-component behavior (e.g. the user can
  configure motor parameters via the GUI and have them persisted in
  firmware NVRAM).

System-level tests are typically SIL scenarios or end-to-end integration
tests that exercise the full stack. SIL is the primary verification surface
for this project.

## Start here

- [[overview|System overview]] — block diagram, two-component architecture,
  external/internal interfaces, operating-mode state machine, persistence,
  plurality. Contains the small set of architectural anchor `sys~` specs
  (standalone device, app as auxiliary, USB CDC transport, mode FSM, NVRAM,
  multi-device).

## Topics

Sub-folders are created when a topic gets its first spec. The topics below
are the anticipated structure; folders that are not yet present have no
specs in them yet.

- `motor-control/` — torque, velocity, position, trajectory tracking
- `estimation/` — state estimation, sensorless operation
- `observability/` — telemetry stream, diagnostics, logging
- `operating-modes/` — system mode state machine (idle / calibrate / run /
  fault), command authority, mode transitions
- `safety/` — protections (overcurrent, overtemp, encoder-loss,
  gate-driver fault), fault recovery
- `power-startup/` — USB-PD negotiation, power-up sequencing,
  brown-out recovery
