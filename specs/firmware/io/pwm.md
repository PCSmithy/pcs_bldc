---
status: draft
tags: [firmware, io, pwm, driver]
---

# PWM io-layer driver

The `IO_PWM` driver is the generic io-layer three-phase PWM abstraction over
`HW_TIM`. It presents normalized per-phase duty commands and a bridge output
enable, addressed by logical phase; each phase maps to a configured HW_TIM
channel and complementary output-compare unit.

See also: [[overview]] (sys~arch_005~1), [[tim]] (HW_TIM supplies the
complementary PWM), [[bridge-actuation]] (sys~mc_004~1).

## Driver configuration and lifecycle

### Initialization and configuration validation
`fw~io_pwm_001~1`

The driver shall validate the supplied configuration and initialize every
configured phase, returning false on any of:

| Element | Rejected when |
|---------|---------------|
| Config  | The config pointer or its phase array is NULL, or the phase count exceeds the available phases. |
| Phase   | Its HW_TIM channel or output-compare unit is out of range. |

Acceptance:
- A valid configuration initializes every phase and returns true.
- Each rejection condition returns false.

Covers:
- sys~arch_005~1

Needs: impl, test

## Bridge actuation

### Per-phase duty command
`fw~io_pwm_002~1`

The driver shall apply a per-phase duty command in [0, 1] to the phase's
output-compare unit as the fraction of the PWM period during which the
phase's primary output is active, returning false for a command outside
[0, 1], an out-of-range phase, or an uninitialized driver.

Acceptance:
- Duty commands of 0, 0.5, and 1 produce compare values of zero, half, and
  the full timer period on the phase's output-compare unit.
- Each rejected command returns false and leaves every compare value
  unchanged.

Covers:
- sys~mc_004~1

Needs: impl, test

### Bridge output enable
`fw~io_pwm_003~1`

The driver shall set and report the bridge output-enable state through the
phase timer's master output enable (fw~hal_tim_008~1): while disabled, every
phase output holds its inactive state and accepted duty commands take effect
on the outputs at re-enable, and the reported state includes a disable forced
by the timer's break input.

Acceptance:
- Disabling holds all phase outputs inactive.
- Duty commands issued while disabled return true and take effect at
  re-enable.
- With no break asserted, the reported state matches the commanded state.
- The reported state reads disabled after a break-input assertion.

Covers:
- sys~mc_004~1

Needs: impl, test
