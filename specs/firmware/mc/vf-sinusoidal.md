---
status: draft
tags: [firmware, mc, app, driver]
---

# V/f sinusoidal method

Open-loop commutation: a forced electrical-angle ramp with a
volts-per-hertz amplitude law. The rotor follows the rotating stator field
without feedback.

See also: [[motor-control-application]] (fw~mc_006~1 dispatches this
method), [[commutation-method-architecture]] (sys~mc_005~1).

### V/f sinusoidal commutation
`fw~mc_010~1`

While active with the bridge enabled, the V/f method shall advance its
electrical angle at the speed target's electrical frequency and command
per-phase duties

d_x = 0.5 + (A / 2) · sin(θ − x · 120°),  x ∈ {0, 1, 2}

where A follows the configured volts-per-hertz slope on the target
frequency, clamped between the configured floor and maximum amplitudes; at
a zero target the angle holds and A is the floor.

Acceptance:
- At a constant nonzero target the three commanded duties are sinusoids at
  the target electrical frequency, 120° apart, centered on 0.5.
- A equals the slope times the frequency magnitude, clamped to
  [floor, maximum].
- At a zero target the duties are constant with A at the floor.

Covers:
- sys~mc_005~1

Needs: impl, test
