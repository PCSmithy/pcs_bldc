---
status: draft
tags: [firmware, hal, tim, driver]
---

# Timer hardware abstraction layer

The `HW_TIM` driver is the generic, reusable hw-layer abstraction over the
MCU's timer peripherals. It presents a counter, output-compare, and trigger
API through a single target-independent header, usable by any consumer.

The driver addresses hardware in two planes. The **peripheral plane** enumerates
the physical timer peripherals (`HW_TIM_PERIPHERAL_*`); each peripheral carries a
counter with a configured direction, period, and tick rate, an optional trigger
output, and an optional break input feeding a master output enable. The
**logical-channel plane** enumerates the output-compare units consumers drive
(`HW_TIM_CHANNEL_*`); each channel names the peripheral it lives on, its role,
and one output-compare unit that drives an output line from a compare value in
raw counts.

See also: [[overview]] (sys~arch_005~1).

## Driver configuration and lifecycle

### Timer initialization and configuration validation
`fw~hal_tim_001~1`

The timer HW driver shall validate the supplied configuration before use and
initialize every configured peripheral and logical channel, starting each
counter with its outputs in their configured inactive state, returning false if
the configuration is rejected by any of:

| Element | Rejected when |
| ------- | ------------- |
| Config | The config pointer, peripheral array, or channel array is NULL, or either the peripheral or channel count exceeds the available count. |
| Peripheral | The configured period or prescaler exceeds the peripheral's counter width, the count direction is unsupported, or a dead-time exceeds the supported range. |
| Channel | The role is not a supported role, the named peripheral is not a configured one, the output-compare unit is out of range, or the initial compare value exceeds the peripheral's period. |

Acceptance:
- A valid config initializes every peripheral and channel, leaves each counter
  advancing and each output inactive, and returns true.
- Each rejection condition returns false.

Covers:
- sys~arch_005~1

Needs: impl, test

## Counter time base

### Counter direction and period
`fw~hal_tim_002~1`

Each peripheral's counter shall advance in its configured direction (up, down,
or center-aligned) over its configured period at a tick rate of the peripheral
clock divided by the configured prescaler.

Acceptance:
- A peripheral configured up counts from zero to its period then rolls over; one
  configured down counts from its period to zero; one configured center-aligned
  counts up to its period then back down.
- The counter advances one step per prescaled clock tick.

Covers:
- sys~arch_005~1

Needs: impl, test

### Free-running counter readout
`fw~hal_tim_003~1`

The driver shall return the present counter value of an addressed peripheral in
raw counts; a read of an uninitialized driver or an out-of-range peripheral
returns false.

Acceptance:
- A read after a known number of elapsed ticks returns the corresponding
  counter value.
- A read of an out-of-range peripheral or before initialization returns false.

Covers:
- sys~arch_005~1

Needs: impl, test

## Output-compare and PWM

### Output-compare unit operation
`fw~hal_tim_004~1`

Each logical channel's output-compare unit shall drive its output line from a
runtime-settable compare value in raw counts under its configured output mode,
and shall be individually enabled or disabled, driving its configured inactive
level while disabled.

Acceptance:
- Setting a channel's compare value changes its output waveform accordingly, and
  the value reads back.
- A disabled channel drives its configured inactive level regardless of compare
  value; enabling it resumes output.
- Setting a compare value above the peripheral's configured period returns false.

Covers:
- sys~arch_005~1

Needs: impl, test

### Complementary outputs with dead-time
`fw~hal_tim_005~1`

A logical channel configured for complementary output shall drive its paired
primary and complementary lines in antiphase, with the peripheral's configured
dead-time inserted before each line activates.

Acceptance:
- A complementary channel's two output lines are antiphase.
- The configured dead-time separates one line's deactivation from its
  complement's activation, in both switching directions.

Covers:
- sys~arch_005~1

Needs: impl, test

## Synchronization and fault handling

### Trigger output for peripheral synchronization
`fw~hal_tim_006~1`

A peripheral shall emit a trigger output on its configured source event, either
the counter update or an output-compare match.

Acceptance:
- A peripheral configured to trigger on its update event emits a trigger at each
  period boundary.
- A peripheral configured to trigger on an output-compare match emits a trigger
  when the counter reaches that unit's compare value.

Covers:
- sys~arch_005~1

Needs: impl, test

### Break input safe-state shutdown
`fw~hal_tim_007~1`

On a peripheral with a break input configured, an asserted break signal shall
force that peripheral's outputs to their configured inactive state and hold them
there while the break remains asserted.

Acceptance:
- Asserting the break input drives every output of the peripheral to its
  inactive level.
- The outputs remain inactive while the break stays asserted; after release,
  restoration follows the master output enable (fw~hal_tim_008~1).

Covers:
- sys~arch_005~1

Needs: impl, test

### Master output enable
`fw~hal_tim_008~1`

The driver shall set, clear, and report a peripheral's master output enable,
which gates every enabled output-compare output on the peripheral simultaneously
while leaving per-channel configuration intact: the outputs hold their inactive
state whenever it is clear — commanded, or cleared by a break event — and it
stays clear until set again.

Acceptance:
- Clearing the master output enable holds every enabled output at its
  inactive state.
- Setting it restores the outputs per the per-channel compare values and
  enables, which are unchanged by the clear.
- After a break-input assertion and release, the reported state reads
  disabled and the outputs stay inactive until it is set again.
- Set, clear, and read on an out-of-range or uninitialized peripheral returns
  false.

Covers:
- sys~arch_005~1

Needs: impl, test
