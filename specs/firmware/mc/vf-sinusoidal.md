---
status: draft
tags: [firmware, mc, app, driver]
---

# V/f sinusoidal method

Open-loop commutation: a commanded electrical angle advances at the
target's electrical frequency and a volts-per-frequency law sets the
voltage vector the modulator applies. The rotor follows the rotating
stator field.

See also: [[motor-control-application]] (fw~mc_015~1 runs this method's
step each PWM cycle), [[modulation]] (fw~mc_013~1 and fw~mc_014~1 turn the
voltage command into duties), [[bridge]] (fw~io_bridge_005~1 supplies the
bus voltage), [[commutation-method-architecture]] (sys~mc_005~1).

### V/f sinusoidal commutation
`fw~mc_010~1`

Each commutation step while active, the V/f method shall advance its
commanded electrical angle θ by ω_e · T_pwm — ω_e the slewed electrical
frequency (fw~mc_017~1), T_pwm the PWM period — and command, through the
inverse Park transform and the modulator, the rotor-frame voltage
(v_d, v_q) = (0, V) at θ with all three phases enabled, where

| Bus voltage      | V                                          |
|------------------|--------------------------------------------|
| ≥ V_bus,min      | K_e · abs(ω_e) + V_boost, clamped to V_max |
| < V_bus,min      | 0                                          |

for K_e the configured electrical back-EMF constant in V·s/rad and V_boost,
V_max, V_bus,min the configured boost, ceiling, and minimum bus voltages.

Acceptance:
- At a constant nonzero ω_e the commanded angle advances by ω_e · T_pwm per
  step and the line-to-line duty differences are sinusoids at ω_e, 120°
  apart.
- At an ω_e where K_e · abs(ω_e) + V_boost exceeds V_max, V equals V_max.
- At ω_e = 0 the angle holds and V equals V_boost.
- Reversing the sign of ω_e reverses the angle progression.
- With the bus voltage below V_bus,min all three duties equal 0.5.

Covers:
- sys~mc_005~1

Needs: impl, test

### Frequency slew limit
`fw~mc_017~1`

The V/f method shall move its commanded electrical frequency toward the
speed target's electrical frequency — the mechanical target times the
configured pole-pair count — by at most a_max · T_pwm per commutation
step, a_max the configured slew rate in rad/s² and T_pwm the PWM period.

Acceptance:
- A step change in the speed target produces a commanded-frequency ramp at
  a_max that settles at the new target.
- A target change smaller than a_max · T_pwm is reached in one step.

Covers:
- sys~mc_005~1

Needs: impl, test
