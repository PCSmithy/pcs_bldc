---
status: draft
tags: [firmware, mc, app, driver]
---

# Sensored sinusoidal method

Sinusoidal commutation on the encoder: a rotor-frame voltage command on
the q axis of the per-cycle rotor angle, applied through the transforms
and the modulator.

See also: [[motor-control-application]] (fw~mc_015~1 runs this method's
step each PWM cycle, fw~mc_019~1 supplies the per-cycle rotor angle and
the alignment offset is fw~mc_012~1), [[modulation]] (fw~mc_013~1, the
inverse Park transform), [[bridge]] (fw~io_bridge_008~1 applies the
voltage vector), [[commutation-method-architecture]] (sys~mc_005~1).

### Sensored sinusoidal commutation
`fw~mc_020~1`

Each commutation step while active, the sensored sinusoidal method shall
command, through the inverse Park transform (fw~mc_013~1) at the per-cycle
rotor electrical angle (fw~mc_019~1) and the bridge voltage-vector command
(fw~io_bridge_008~1), the rotor-frame voltage (v_d, v_q) = (0, V) with all
three phases enabled, where

V = sgn(ω*) · min(|ω*| / ω_max, 1) · V_max

for ω* the speed target, ω_max the channel's configured maximum mechanical
velocity, and V_max its configured maximum voltage.

Acceptance:
- The stator voltage vector recovered from the three duties lies 90° ahead
  of the rotor electrical angle for a positive target and 90° behind it for
  a negative one, at magnitude |V|, over a sweep of targets through ω_max.
- At a constant speed the line-to-line duty differences are sinusoids at
  the electrical frequency, 120° apart, within 2% of their amplitude.

Covers:
- sys~mc_005~1

Needs: impl, test
