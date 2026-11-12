---
status: draft
tags: [app, views]
---

# Command widget

A widget that sends one command to the board: a mode, one setpoint in
engineering units, and its limits; or a stop.

See also: [[workspace]] (widgets and their persisted configuration),
[[../conn/command]] (the request it sends).

### Command widget
`app~views_024~1`

A Command widget shall offer a mode selector over the board's modes, a
setpoint field and the mode's limit fields in the units and bounds per
the table — the bounds the reported `MotorInfo` values
(`app~conn_004~1`), the velocity, rate and acceleration limits held at
zero or above, a zero limit shown as the table's zero meaning, a mode of
another setpoint kind starting the setpoint and limits from zero and a
mode of the same kind keeping them within its bound — a Send
control that sends a `Command` of that mode and one setpoint at time
zero, and a Stop control that sends `MODE_OFF` with no setpoints, the
selection kept, available while a Send is in flight, the latest send's
verdict shown beside the controls until the next send; the fields, Send,
and Stop unavailable while no session is connected (`app~conn_001~1`),
and the widget's mode, setpoint, and limits held as workspace
configuration.

| Mode | Setpoint | Bound | Limit fields (zero meaning) |
|------|----------|-------|-----------------------------|
| Six-step, sinusoidal, field-oriented velocity | rad/s | maximum velocity | rate, rad/s² (step) |
| Field-oriented torque | Nm | maximum torque | rate, Nm/s (step) |
| Field-oriented position | rad | maximum position | velocity, rad/s, bounded at maximum velocity (max); acceleration, rad/s² (step); velocity at the point, rad/s, bounded at the velocity limit (stop) |
| Off | none | — | — |

Acceptance:
- Selecting the torque method relabels the setpoint field to Nm and
  bounds it at the motor's maximum torque; selecting a velocity method
  relabels it to rad/s and bounds it at the maximum velocity.
- From field-oriented velocity at 18 rad/s, sinusoidal keeps 18 rad/s
  and its limit; the torque method starts from zero.
- Selecting the position method relabels the setpoint field to rad and
  shows the velocity-limit, acceleration-limit and velocity-at-the-point
  fields.
- Send with 5 rad/s and a limit of zero sends a `Command` of the
  selected mode with one setpoint at 0 µs, 5 rad/s, limit 0.
- A rejected command shows its `cause` beside Send; an accepted one
  shows accepted.
- Stop sends `MODE_OFF` with no setpoints; the selected mode and
  setpoint stay; pressed while a Send is in flight, it goes out and its
  verdict stands.
- A typed limit of -1 holds at zero.
- Disconnected, the fields, Send, and Stop are unavailable; reconnected,
  the widget's last mode, setpoint, and limits are shown.

Covers:
- sys~ops_003~1
- sys~obs_011~1

Needs: impl, test
