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

- `mc/` — sensored & sensorless motor control (anchor `sys~mc_001`), torque,
  velocity, position, trajectory tracking; gate-driver management (anchors
  `sys~mc_002` configuration, `sys~mc_003` fault observability)
- `est/` — state estimation, sensorless operation
- `conn/` — device↔app protocol: schema-defined messages
  (`sys~conn_001`), message framing (`sys~conn_002`), request
  acknowledgement (`sys~conn_003`)
- `obs/` — observability: periodic status telemetry (`sys~obs_001`),
  signal selection from firmware debug information (`sys~obs_002`),
  firmware build identity + gate (`sys~obs_003`/`_004`), signal trace
  (`sys~obs_005`), signal write (`sys~obs_006`), text log stream
  (`sys~obs_007`)
- `ops/` — system mode state machine (idle / calibrate / run / fault),
  command authority (`sys~ops_002`, [[command-authority]]), mode
  transitions (anchor `sys~ops_001` lives in [[overview]])
- `safety/` — protections (overcurrent, overtemp, encoder-loss,
  gate-driver fault), fault recovery
- `pd/` — USB-PD sink power monitoring (anchor `sys~pd_001`), power-up
  sequencing, brown-out recovery
