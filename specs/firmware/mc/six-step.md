---
status: draft
tags: [firmware, mc, app, driver]
---

# Six-step encoder-commutated method

Sensored block commutation: the rotor electrical angle selects which two
phases conduct; the third floats.

See also: [[motor-control-application]] (fw~mc_015~1 runs this method's
step each PWM cycle, fw~mc_019~1 supplies the rotor electrical angle and
the alignment offset is fw~mc_012~1), [[bridge]] (`fw~io_bridge_002`
duties; per-phase float via `fw~io_bridge_004` output disable).

### Six-step commutation
`fw~mc_011~1`

While active with the bridge enabled, the six-step method shall apply the
commutation pattern of the sector containing the rotor electrical angle
(fw~mc_019~1) advanced by the configured lead angle in the direction of
the signed speed target, at a
duty proportional to the target magnitude clamped to the configured
maximum:

| Sector (electrical) | Duty d | Duty 0 | Floating |
|---------------------|--------|--------|----------|
| 0°–60°     | U | W | V |
| 60°–120°   | V | W | U |
| 120°–180°  | V | U | W |
| 180°–240°  | W | U | V |
| 240°–300°  | W | V | U |
| 300°–360°  | U | V | W |

each pattern's stator field lying at its sector's centre.

Acceptance:
- In each sector, the table row's duty-d phase and duty-0 phase are driven
  and the floating phase's outputs are disabled.
- Reversing the target sign reverses the sector progression.
- The applied duty is proportional to the target magnitude, clamped to the
  configured maximum.

Covers:
- sys~mc_005~1

Needs: impl, test
