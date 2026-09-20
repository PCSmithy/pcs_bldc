---
status: draft
tags: [system, conn]
---

# Link throughput test

### Link throughput test
`sys~conn_004~1`

On request, the firmware shall stream a commanded count of frames of a
commanded payload size, each carrying a sequence number.

Acceptance:

- A request for N frames of S bytes yields N frames of S bytes with
  sequence numbers 0 through N − 1 at the host.

See also: [[framing]]

Covers:

- (project goal: README.md, "Modeling, simulation, and observability
  infrastructure" — raw sensor data at meaningful rates over the
  USB link.)

Needs: fw, test
