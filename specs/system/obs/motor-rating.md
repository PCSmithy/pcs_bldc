---
status: draft
tags: [system, obs]
---

# Motor rating report

### Motor rating report
`sys~obs_011~1`

The device shall report, on a host request, the ratings its motor control
is configured with — torque constant, maximum current, maximum torque, and
maximum velocity — so the host bounds its setpoints to them.

Acceptance:

- The report carries the four configured ratings.
- The host's setpoint controls take the reported maxima as their bounds.

Covers:

- (project goal: README.md, "Modeling, simulation, and observability
  infrastructure" — live inspection, command, and tuning via the desktop
  visualizer / control app.)

Needs: fw, app, test
