---
status: draft
tags: [app, conn]
---

# Board commands

The core's typed path for the board's command plane: the `Command` and
`GetMotorInfo` requests of [[../../firmware/conn/server]], sent over the
session ([[session]], [[wire]]) and answered with a verdict or a result.

### Command dispatch
`app~conn_004~1`

The core shall send, on the UI's request, a `Command` — a mode and a
list of setpoints, each a time and a speed or torque target with a rate
limit, a position with a velocity and an acceleration limit and a
velocity at the point, or a stop — and a `GetMotorInfo` request,
resolving each to the UI with the
`Response` verdict and `cause` (`app~conn_003~1`) or the `MotorInfo`
result, a `Command` the board rejects as not yet started
(`fw~conn_server_007~1`) resent after 2 ms, up to three sends, and a
`Command` whose envelope exceeds the board's 448-byte frame payload
refused unsent with a cause naming its size.

Acceptance:
- A `Command` with two setpoints encodes with the mode, both setpoints'
  times, targets, and limits, in order; a stop setpoint encodes as the
  `stop` target.
- A rejected `Command` resolves to the UI with `accepted` clear and the
  board's `cause`; an accepted one with `accepted` set.
- A `GetMotorInfo` resolves with the five `MotorInfo` fields.
- A `Command` rejected as not yet started and then accepted resolves
  accepted after two sends.
- A `Command` of 20 position setpoints with all their limits resolves
  with `frame too large` and writes nothing.

Covers:
- sys~ops_003~1
- sys~obs_011~1

Needs: impl, test
