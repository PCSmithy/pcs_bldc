---
status: draft
tags: [firmware, mc, app, driver]
---

# Six-step encoder-commutated method

Sensored block commutation: the rotor electrical angle — motor shaft angle
corrected by a captured alignment offset and scaled by the pole-pair
count — selects which two phases conduct; the third floats.

See also: [[motor-control-application]] (fw~mc_006~1 dispatches this
method), [[bridge]] (fw~io_bridge_002 duties; per-phase float via
fw~io_bridge_004 output disable).

### Six-step commutation
`fw~mc_011~1`

While active with the bridge enabled, the six-step method shall apply the
commutation pattern of the sector containing the rotor electrical angle —
derived from the motor shaft angle, the stored alignment offset
(fw~mc_012~1), and the configured pole-pair count — advanced by the
configured lead angle in the direction of the signed speed target, at a
duty proportional to the target magnitude clamped to the configured
maximum:

| Sector (electrical) | Duty d | Duty 0 | Floating |
|---------------------|--------|--------|----------|
| 0°–60°     | U | V | W |
| 60°–120°   | U | W | V |
| 120°–180°  | V | W | U |
| 180°–240°  | V | U | W |
| 240°–300°  | W | U | V |
| 300°–360°  | W | V | U |

Acceptance:
- In each sector, the table row's duty-d phase and duty-0 phase are driven
  and the floating phase's outputs are disabled.
- Reversing the target sign reverses the sector progression.
- The applied duty is proportional to the target magnitude, clamped to the
  configured maximum.

Covers:
- sys~mc_005~1

Needs: impl, test

### Alignment offset capture
`fw~mc_012~1`

On its first bridge enable after power-up, the six-step method shall drive
the 0°–60° sector pattern at the configured alignment duty for the
configured dwell time, then capture the motor shaft angle as the alignment
offset on which all subsequent enables commutate.

Acceptance:
- The first enable applies the alignment pattern for the dwell time, then
  captures the offset, then begins commutation.
- Subsequent enables commutate immediately with the stored offset.

Covers:
- sys~mc_005~1

Needs: impl, test
