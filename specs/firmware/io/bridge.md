---
status: draft
tags: [firmware, io, bridge, driver]
---

# Bridge io-layer driver

The `IO_bridge` driver is the generic io-layer three-phase motor-bridge
abstraction over `HW_TIM`. It presents normalized per-phase duty commands, a
per-phase output enable, and a whole-bridge output enable, addressed by logical
bridge and phase; each phase maps to a configured HW_TIM logical channel, and a
bridge's three phases share one HW_TIM peripheral whose master output enable
gates the bridge.

See also: [[overview]] (sys~arch_005~1), [[tim]] (HW_TIM supplies the
complementary PWM), [[bridge-actuation]] (sys~mc_004~1).

## Driver configuration and lifecycle

### Initialization and configuration validation
`fw~io_bridge_001~1`

The driver shall validate the supplied configuration and initialize every
configured bridge, returning false on any of:

| Element | Rejected when |
|---------|---------------|
| Config  | The config pointer or its channel array is NULL, or the bridge count exceeds the available bridges. |
| Phase   | A phase's HW_TIM channel is out of range. |
| Bridge  | The three phases do not resolve to one shared HW_TIM peripheral. |

Acceptance:
- A valid configuration initializes every bridge, enables each phase's
  output-compare unit, leaves the master output enable clear, and returns true.
- Each rejection condition returns false.

Covers:
- sys~arch_005~1

Needs: impl, test

## Bridge actuation

### Per-phase duty command
`fw~io_bridge_002~1`

The driver shall apply a per-phase duty command in [0, 1] to the phase's
output-compare unit as the fraction of the PWM period during which the phase's
primary output is active, returning false for a command outside [0, 1], an
out-of-range bridge or phase, or an uninitialized driver.

Acceptance:
- Duty commands of 0, 0.5, and 1 produce compare values of zero, half, and the
  full timer period on the phase's output-compare unit.
- Each rejected command returns false and leaves every compare value unchanged.

Covers:
- sys~mc_004~1

Needs: impl, test

### Bridge output enable
`fw~io_bridge_003~1`

The driver shall set and report the whole-bridge output-enable state through the
phases' shared master output enable (fw~hal_tim_008~1): while disabled, every
phase output holds its inactive state and accepted duty commands take effect on
the outputs at re-enable, and the reported state includes a disable forced by
the peripheral's break input.

Acceptance:
- Disabling holds all phase outputs inactive.
- Duty commands issued while disabled return true and take effect at re-enable.
- With no break asserted, the reported state matches the commanded state.
- The reported state reads disabled after a break-input assertion.

Covers:
- sys~mc_004~1

Needs: impl, test

### Per-phase output enable
`fw~io_bridge_004~1`

The driver shall enable or disable one phase's output independently, a disabled
phase holding both its gate lines at the inactive level while the other phases
keep driving.

Acceptance:
- Disabling one phase holds that phase's outputs inactive and leaves the other
  phases' outputs driving.
- Re-enabling the phase restores its output.
- A command on an out-of-range phase returns false.

Covers:
- sys~mc_004~1

Needs: impl, test
