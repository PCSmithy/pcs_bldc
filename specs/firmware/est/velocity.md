---
status: draft
tags: [firmware, est, velocity]
---

# Velocity estimation

The rotor mechanical-velocity estimate derived from the encoder angle
([[encoder]]), consumed by status publication ([[../obs/status]]).

### Encoder-derived velocity estimate
`fw~est_velocity_001~1`

The firmware shall maintain an estimate of rotor mechanical velocity,
updated each 1 ms tick from successive encoder angles
(`fw~est_encoder_001~1`).

Acceptance:
- Under a constant true angle rate, the estimate converges to within
  1 % of that rate inside 50 ms.
- Through the encoder's 0/2π angle wrap, every per-tick estimate stays
  within 10 % of a constant true rate.

Covers:
- sys~obs_001~1

Needs: impl, test
