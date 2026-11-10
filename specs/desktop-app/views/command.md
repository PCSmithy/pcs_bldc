---
status: draft
tags: [app, views]
---

# Command widget

A widget that sends one command to the board: a mode, one setpoint in
engineering units, and its rate limit; or a stop.

See also: [[workspace]] (widgets and their persisted configuration),
[[../conn/command]] (the request it sends).

### Command widget
`app~views_024~1`

A Command widget shall offer a mode selector over the board's modes, a
setpoint field and a rate limit field in the mode's units and bounds per
the table — the bounds the reported ratings (`app~conn_004~1`,
`MotorInfo`), a zero limit shown as "step" — a Send control that sends
a `Command` of that mode and one setpoint at time zero, and a Stop
control that sends `MODE_OFF` with no setpoints, the selection kept, the
verdict shown beside the controls until the next send; the fields, Send,
and Stop unavailable while no session is connected (`app~conn_001~1`),
and the widget's mode, setpoint, and limit held as workspace
configuration.

| Mode | Setpoint | Bound | Rate limit |
|------|----------|-------|------------|
| Six-step, sinusoidal, field-oriented velocity | rad/s | maximum velocity | rad/s² |
| Field-oriented torque | Nm | maximum torque | Nm/s |
| Off | none | — | — |

Acceptance:
- Selecting the torque method relabels the setpoint field to Nm and
  bounds it at the motor's maximum torque; selecting a velocity method
  relabels it to rad/s and bounds it at the maximum velocity.
- Send with 5 rad/s and a limit of zero sends a `Command` of the
  selected mode with one setpoint at 0 µs, 5 rad/s, limit 0.
- A rejected command shows its `cause` beside Send; an accepted one
  shows accepted.
- Stop sends `MODE_OFF` with no setpoints; the selected mode and
  setpoint stay.
- Disconnected, the fields, Send, and Stop are unavailable; reconnected,
  the widget's last mode, setpoint, and limit are shown.

Covers:
- sys~ops_003~1
- sys~obs_011~1

Needs: impl, test
