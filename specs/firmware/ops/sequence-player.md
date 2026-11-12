---
status: draft
tags: [firmware, ops, app]
---

# Sequence player

The `app_sequencer` module: the host's timed setpoint sequence
(`fw~conn_server_007~1`), held in a table of configured capacity and
written to the motor-control application on the 1 ms cycle.

See also: [[../conn/server]] (the command that delivers a sequence),
[[../mc/motor-control-application]] (the targets and setpoints it writes),
[[setpoint-sequence]] (the system requirement).

### Sequence playback
`fw~ops_001~1`

The sequence player shall hold one sequence of up to its configured
capacity of setpoints — each a time in microseconds from the sequence's
start and one of the kinds below — and, taking the start as the 1 ms
pass following the sequence's acceptance, write each setpoint at the
first pass whose time from the start is at or past the setpoint's, in
list order, as its row says:

| Setpoint | Written as |
|----------|------------|
| A speed in rad/s or a torque in Nm, with its rate limit | The speed or torque target with its rate limit (`fw~mc_027~1`) |
| A position in rad, with its velocity and acceleration limits and its velocity at the point | The position setpoint (`fw~mc_030~1`) |
| A stop | The `MODE_OFF` request (`fw~mc_006~1`) |

Acceptance:
- Speed setpoints at 0, 250 000, and 500 000 µs are written at passes 0,
  250, and 500 from the start, each within one pass.
- Two setpoints at the same time are written in the same pass, in list
  order.
- A sequence of 24 setpoints, the capacity on this board, is held and
  written in full.
- A position setpoint at 100 000 µs is written with its limits and its
  velocity at the point at pass 100 from the start, within one pass.
- After the last setpoint is written, no further write occurs.
- A stop at 300 000 µs requests `MODE_OFF` at pass 300 from the start,
  within one pass, writing no target.

Covers:
- sys~ops_003~1

Needs: impl, test

### Sequence replacement and abort
`fw~ops_002~1`

The sequence player shall discard the running sequence's unwritten
setpoints when a `Command` is accepted (`fw~conn_server_007~1`) or a
fault latches (`fw~safety_001~1`, `fw~safety_002~1`), the targets already
written standing.

Acceptance:
- A command accepted between two setpoints leaves the second unwritten.
- `MODE_OFF` with an empty list ends the running sequence; no further
  setpoint is written.
- A fault latched between two setpoints leaves the second unwritten,
  through and after the fault's clearing.

Covers:
- sys~ops_003~1

Needs: impl, test
