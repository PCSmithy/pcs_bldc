---
status: draft
tags: [system, obs]
---

# CPU load visibility

### CPU load visibility
`sys~obs_010~1`

The system shall expose the processor time taken by each firmware task,
by the idle task, and by the PWM-cycle interrupt as traceable signals
(`sys~obs_005~1`), and show the operator each one's share of the
processor and the idle margin as they change.

Acceptance:

- Over any 1 s window the task and idle signals' advances sum to the
  window's duration within 2 %.
- With the drive off and no trace watches, the idle share shown exceeds
  every other share.
- Adding four one-cycle watches changes the shown shares within 2 s.

See also: [[signal-trace]]

Covers:

- (project goal: README.md, "Modeling, simulation, and observability
  infrastructure" — control loop internals and raw data at meaningful
  rates; the loop's own cost is part of that picture.)

Needs: fw, app, test
