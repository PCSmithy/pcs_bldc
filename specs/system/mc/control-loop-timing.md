---
status: draft
tags: [system, mc]
---

# Control-loop timing

### Control-loop timing
`sys~mc_006~1`

Each PWM cycle, the firmware shall complete the commutation step within
20 µs of the cycle callback's entry and the whole callback within 40 µs
of it, exposing the observed maximum of each duration for host readout.

Acceptance:

- Over 60 s at the maximum speed target with four 4-byte one-cycle
  watches and 28 4-byte watches at 10 ms installed (`sys~obs_005~1`),
  the exposed maxima read 20 µs or less for the step and 40 µs or less
  for the callback.

See also: [[commutation-method-architecture]]

Covers:

- (project goal: README.md, "Field-oriented motor control" — the
  PWM-synchronous control loop the FOC inner loop runs in.)

Needs: fw, test
