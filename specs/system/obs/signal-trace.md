---
status: draft
tags: [system, obs]
---

# Signal trace

### Signal trace
`sys~obs_005~1`

The firmware shall stream samples of host-selected memory locations
(`sys~obs_002~1`) over the protocol (`sys~conn_001~1`), sampling each
location at its host-assigned period of one PWM cycle, 1 ms, or 10 ms —
at most 4 locations at the one-cycle period — capturing the locations
that share a period as one coherent snapshot each time it falls due, and
batching samples with the PWM-cycle index of their capture.

Acceptance:

- A 32-bit counter the firmware increments each PWM cycle, traced at one
  cycle, arrives with consecutive values at consecutive cycle indices.
- Two variables the firmware updates together each cycle, traced at one
  cycle, arrive with mutually consistent values in every sample.
- Signals assigned the one-cycle, 1 ms, and 10 ms periods trace
  concurrently, each at its own period.
- A fifth one-cycle location is rejected.

See also: [[signal-selection]], [[identity-gate]]

Covers:

- (project goal: README.md, "Modeling, simulation, and observability
  infrastructure" — control loop internals, references, estimator
  states, raw sensor data at meaningful rates.)

Needs: fw, app, test
