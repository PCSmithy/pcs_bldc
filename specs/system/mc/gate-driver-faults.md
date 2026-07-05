---
status: draft
tags: [system, mc]
---

# Gate-driver fault observability

### Gate-driver fault observability
`sys~mc_003~1`

The firmware shall make each STSPIN32G4 gate-driver fault condition (VDS
protection, thermal shutdown, VCC undervoltage, reset, lock state) observable
to firmware consumers.

Acceptance:
- Each exposed fault condition matches the device's physical state.

Covers:
- (project goal: README.md, "Field-oriented motor control" — the power stage
  that FOC actuates, operated within defined limits with faults observable.)

Needs: fw, test
