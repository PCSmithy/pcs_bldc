---
status: draft
tags: [system, obs]
---

# Signal trace

### Signal trace
`sys~obs_005~1`

The firmware shall stream samples of host-selected memory locations
(`sys~obs_002~1`) over the protocol (`sys~conn_001~1`), sampling each
location at its host-assigned period of 1 ms, 10 ms, or 100 ms,
capturing locations that share a sampling instant as one coherent
snapshot, and batching samples with the millisecond timestamp of their
capture.

Acceptance:

- A 32-bit counter the firmware increments each millisecond, traced at
  1 ms, arrives with consecutive values at consecutive timestamps.
- Two variables the firmware updates together each millisecond, traced
  at 1 ms, arrive with mutually consistent values in every sample.
- Signals assigned 1 ms, 10 ms, and 100 ms periods trace concurrently,
  each at its own period.

See also: [[signal-selection]], [[identity-gate]]

Covers:

- (project goal: README.md, "Modeling, simulation, and observability
  infrastructure" — control loop internals, references, estimator
  states, raw sensor data at meaningful rates.)

Needs: fw, app, test
