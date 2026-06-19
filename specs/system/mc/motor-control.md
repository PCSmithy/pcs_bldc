---
status: draft
tags: [system, mc]
---

# Sensored and sensorless motor control

### Sensored and sensorless motor control
`sys~mc_001~1`

The firmware shall drive the BLDC motor under closed-loop field-oriented
control using rotor position obtained either from the magnetic encoder
(sensored) or from a model-based estimator (sensorless).

Acceptance:
- A run tracks a commanded setpoint under field-oriented control with rotor
  position supplied by the encoder.
- A run tracks a commanded setpoint under field-oriented control with rotor
  position supplied by the estimator and encoder feedback disabled.

Covers:
- (project goal: README.md, "Field-oriented motor control" — inner FOC torque
  loop, both sensored and sensorless.)

Needs: fw, test
