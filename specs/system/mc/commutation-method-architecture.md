---
status: draft
tags: [system, mc]
---

# Commutation method architecture

### Commutation method architecture
`sys~mc_005~1`

The firmware shall drive the motor through one of a set of selectable
commutation methods — each receiving the speed target, motor shaft angle,
and phase currents, and producing the three-phase bridge duty commands —
with the active method selectable only while the bridge is disabled, and
bridge enable permitted only while the gate driver reports operational.

Acceptance:
- Each registered method, while active, drives the bridge duty commands
  from its inputs.
- A method-selection action while the bridge is enabled leaves the active
  method unchanged.
- With the gate driver not reporting operational, a bridge enable request
  leaves the bridge disabled.

Covers:
- (project goal: README.md, "Field-oriented motor control" — first-spin
  commutation methods exercising the actuation path ahead of the FOC
  inner loop.)

Needs: fw, test
