---
status: draft
tags: [firmware, obs, status]
---

# Status publication

Periodic device status to the host: a `Status` message assembled from
the motor-control snapshot and the bus measurements.

See also: [[../conn/server]] (the module publishing it).

### Status publication
`fw~obs_status_001~1`

While a host holds the device's serial port open
(`fw~conn_serial_005~1`), the firmware shall publish a `Telemetry`
message (`fw~conn_proto_001~1`) every 100 ms — within ± 2 % over any
10-second window — reporting, as read at publication: the motor-control
mode and drive state (`fw~mc_009~1`), the measured rotor velocity
(`fw~est_velocity_001~1`), the velocity setpoint, the bus voltage, the
bus current, and the board's milliseconds since boot.

Acceptance:
- Over a 10-second connected window, 100 ± 2 `Telemetry` messages
  arrive, timestamps advancing.
- A mode change, drive-state change, or setpoint change appears in the
  next published `Telemetry`.

Covers:
- sys~obs_001~1

Needs: impl, test
