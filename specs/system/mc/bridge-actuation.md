---
status: draft
tags: [system, mc]
---

# Bridge actuation

### Bridge actuation
`sys~mc_004~1`

The firmware shall actuate the three-phase power bridge through complementary
PWM with runtime-settable per-phase duty cycle taking effect within one PWM
period, and a bridge output enable, holding every gate-drive signal inactive
— regardless of duty commands — from deassertion of the output enable or
assertion of the gate driver's fault line until firmware re-enables the
output.

Acceptance:
- A per-phase duty command is reflected on that phase's gate-drive PWM within
  one PWM period.
- With the output enable deasserted, all six gate-drive signals hold their
  inactive level while duty commands continue.
- Gate-driver fault assertion forces all six gate-drive signals inactive.
- After the fault deasserts, the gate-drive signals stay inactive until
  firmware re-enables the output.

Covers:
- (project goal: README.md, "Field-oriented motor control" — the actuation
  path FOC modulates.)

Needs: fw, test
