---
status: draft
tags: [system, mc]
---

# Sensored and sensorless motor control

### Sensored motor control
`sys~mc_001~1`

The firmware shall drive the BLDC motor under closed-loop field-oriented
control using rotor position obtained from the magnetic encoder.

Acceptance:
- A run tracks a commanded setpoint under field-oriented control with rotor
  position supplied by the encoder.

Covers:
- (project goal: README.md, "Field-oriented motor control" — inner FOC torque
  loop, both sensored and sensorless.)

Needs: fw, test

### Sensorless motor control
`sys~mc_007~1`

The firmware shall drive the BLDC motor under closed-loop field-oriented
control using rotor position obtained from a model-based estimator, with no
encoder feedback.

Acceptance:
- A run tracks a commanded setpoint under field-oriented control with rotor
  position supplied by the estimator and encoder feedback disabled.

Covers:
- (project goal: README.md, "Field-oriented motor control" — inner FOC torque
  loop, both sensored and sensorless.)

Needs: fw, test
