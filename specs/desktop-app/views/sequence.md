---
status: draft
tags: [app, views]
---

# Sequence widget

A widget that edits a timed setpoint sequence as a line chart, runs it on
the board, and shows its progress.

See also: [[workspace]] (widgets and their persisted configuration),
[[command]] (the one-setpoint form), [[../conn/command]] (the request it
sends), [[timeline]] (the shared time axis the progress marker follows).

### Sequence editor
`app~views_025~1`

A Sequence widget shall edit a sequence under one mode as points on a
chart — time from start on the horizontal axis over the sequence's
configured duration, setpoint on the vertical axis bounded by the
reported maximum velocity or maximum torque for the mode
(`app~conn_004~1`, `MotorInfo`) — per the gestures below, the chart
drawing the written targets as a zero-order hold between points and, for
each point with a nonzero rate limit, the target in effect as a ramp at
that limit from the previous value (`fw~mc_027~1`).

| Gesture | Behavior |
|---------|----------|
| Click on empty chart | Adds a point at the pointer's time and value |
| Click on a point | Selects it, marked; its time, value, and rate limit shown in an editable tooltip, a typed time or value moving it as a drag does |
| Drag a point | Moves it, clamped to the bounds and to between its neighbours' times |
| Delete with a point selected | Removes the point |

Acceptance:
- A click at (200 ms, 8 rad/s) adds a point there; dragging it to
  (300 ms, 12 rad/s) moves it; dragging past the maximum velocity leaves
  it at the maximum; dragging past the next point's time leaves it at that
  time.
- Typing 250 and 9 into the selected point's time and value fields moves
  it to (250 ms, 9 rad/s); a typed value past the maximum velocity holds
  at the maximum.
- Two points at (0 ms, 0) and (200 ms, 10 rad/s) draw a step at 200 ms;
  with the second point's limit set to 50 rad/s², a ramp from 0 at 200 ms
  reaching 10 rad/s at 400 ms is drawn over the step.
- Selecting the torque method relabels the vertical axis to Nm and bounds
  it at the maximum torque.

Covers:
- sys~ops_003~1

Needs: impl, test

### Sequence run
`app~views_026~1`

A Sequence widget shall send its sequence as one `Command` on Run
(`app~conn_004~1`) — every point in time order, then a stop at the
sequence's duration, the stop one of the player's capacity
(`fw~ops_001~1`) — showing the verdict, and on acceptance mark the run:
on its chart a marker advancing with the board's reported elapsed time
(`Telemetry`) while it reports the sequence running and holding at the
last reported position once it does not, and on the shared timeline
(`app~views_008~1`) a mark at the sample time current at acceptance; Stop
sending a `Command` of `MODE_OFF` with no setpoints; Run and Stop
unavailable while no session is connected (`app~conn_001~1`).

Acceptance:
- Run sends one `Command` carrying the editor's mode, every point in
  time order, and a stop at the duration; a rejected run shows its
  `cause` and no marker.
- While telemetry reports the sequence running, the chart marker sits at
  the reported elapsed time within one telemetry period; when it reports
  not running, the marker holds.
- Stop sends `MODE_OFF` with no setpoints.
- The timeline carries a mark at the sample time current when the run was
  accepted.

Covers:
- sys~ops_003~1

Needs: impl, test

### Sequence persistence
`app~views_027~1`

A Sequence widget shall hold its mode, duration, and points — each
point's time, value, and rate limit — as workspace configuration
(`app~views_004~1`), restored on restart, and offer its sequences by
name: Save as naming the current sequence, Load replacing the editor's
contents with a named one, the named sequences held in the workspace
configuration.

Acceptance:
- Points, their limits, the mode, and the duration survive an app
  restart.
- Save as "step 10" then Load "step 10" after editing restores the saved
  points.
- A named sequence survives an app restart and is offered to a new
  Sequence widget.

Covers:
- sys~ops_003~1

Needs: impl, test
