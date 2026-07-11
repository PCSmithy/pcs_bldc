---
status: draft
tags: [system, safety]
---

# Loss-of-position protection

### Loss-of-position protection
`sys~safety_002~1`

A sensored drive depends on a valid rotor position; commutating on a stale or
corrupt reading drives the bridge the wrong way. The firmware shall detect a
persistently invalid rotor-position sensor, disable the bridge, and latch a
fault held until an explicit user action clears it.

Acceptance:
- A persistently invalid rotor-position sensor disables the bridge and latches
  the fault.
- The latch persists once the sensor recovers; the user clear action releases
  it.

Covers:
- (project goal: README.md, "Field-oriented motor control" — protection
  keeping bridge operation within design limits.)

Needs: fw, test
