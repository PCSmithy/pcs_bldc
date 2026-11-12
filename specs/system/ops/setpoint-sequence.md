---
status: draft
tags: [system, ops]
---

# Setpoint sequence

See also: [[command-authority]]

### Setpoint sequence
`sys~ops_003~1`

The system shall accept from the host one sequence of timed setpoints
under one commutation method — each setpoint carrying its time from the
sequence's start and either a speed or torque target with its rate limit
or a position with its velocity and acceleration limits and its velocity
at the point, a newly
accepted sequence replacing the running one — and apply each setpoint on
the device's clock at its time, the sequence playing on when the host
detaches.

Acceptance:

- A sequence of speed targets at 0, 250, and 500 ms is applied at those
  times from its acceptance within 2 ms each, measured on the device,
  with the host silent after acceptance.
- A sequence accepted while another runs replaces it from the new
  sequence's first setpoint.
- A sequence whose host detaches after acceptance applies its remaining
  setpoints at their times.
- The host shows the device's verdict on a sent sequence and, while the
  device reports it running, its elapsed time.

Covers:

- (project goal: README.md, "Modeling, simulation, and observability
  infrastructure" — live inspection, command, and tuning via the desktop
  visualizer / control app.)

Needs: fw, app, test
