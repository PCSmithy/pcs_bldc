---
status: draft
tags: [system, safety]
---

# Overcurrent protection

### Overcurrent protection
`sys~safety_001~1`

The firmware shall disable the three-phase bridge and latch a fault within
2 ms of any phase current magnitude exceeding 5 A or the VBUS current
exceeding 3 A, holding the bridge disabled until an explicit user action
clears the fault.

Acceptance:
- A phase current magnitude exceeding 5 A disables the bridge and latches
  the fault within 2 ms.
- A VBUS current exceeding 3 A disables the bridge and latches the fault
  within 2 ms.
- With currents back below both thresholds, the bridge stays disabled
  until the user clear action.

Covers:
- (project goal: README.md, "Field-oriented motor control" — protection
  keeping bridge operation within design limits.)

Needs: fw, test
