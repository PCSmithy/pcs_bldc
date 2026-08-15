---
status: draft
tags: [system, conn]
---

# Message framing

### Message framing
`sys~conn_002~1`

Protocol messages (`sys~conn_001~1`) shall traverse the transport in
delimited frames validated by a CRC-32, a receiver discarding each
invalid frame with subsequent frames decoding normally.

Acceptance:

- Deleting or corrupting bytes mid-stream, in either direction, loses at
  most the frames containing them; subsequent frames decode normally.

See also: [[protocol]]

Covers:

- (project goal: README.md, "Modeling, simulation, and observability
  infrastructure" — live inspection, command, and tuning via the desktop
  visualizer / control app.)

Needs: fw, app, test
