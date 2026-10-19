---
status: draft
tags: [system, obs]
---

# CPU load visibility

### CPU load visibility
`sys~obs_010~1`

The system shall show the operator the share of the processor taken by
each firmware task, by the idle task, and by the PWM-cycle interrupt
over the latest second, refreshed within 2 s of a change, computed from
cumulative execution-time signals the firmware exposes to the trace
(`sys~obs_005~1`): one per task and one for the interrupt, the task and
idle signals together accounting for the elapsed time, interrupt time
inside the task it interrupts.

Acceptance:

- Over any 1 s window the task and idle signals' advances sum to the
  window's duration within 2 %.
- With the motor not driven and only these signals traced, the idle
  share shown exceeds every other share.
- Adding four 20 kHz trace watches raises the interrupt's shown share
  within 2 s.

See also: [[signal-trace]]

Covers:

- (project goal: README.md, "Modeling, simulation, and observability
  infrastructure" — control loop internals and raw data at meaningful
  rates.)

Needs: fw, app, test
