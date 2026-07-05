---
status: draft
tags: [system, mc]
---

# Gate-driver configuration

### Gate-driver configuration
`sys~mc_002~1`

The firmware shall operate the STSPIN32G4 gate driver at a defined
power-stage configuration.

Acceptance:
- The gate driver's configuration registers contain the defined values,
  verified by readback over I2C.

Covers:
- (project goal: README.md, "Field-oriented motor control" — the power stage
  that FOC actuates, operated within defined limits.)

Needs: fw, test
