---
status: draft
tags: [system, conn]
---

# Request acknowledgement

### Request acknowledgement
`sys~conn_003~1`

The firmware shall answer every received protocol request
(`sys~conn_001~1`) with a response reporting the request accepted, or
rejected with a cause.

Acceptance:

- A well-formed request receives an accepted response.
- A malformed or invalid-parameter request receives a rejected response
  naming the cause.

See also: [[protocol]]

Covers:

- (project goal: README.md, "Modeling, simulation, and observability
  infrastructure" — live inspection, command, and tuning via the desktop
  visualizer / control app.)

Needs: fw, test
