---
status: draft
tags: [firmware, hal, tim, driver]
---

# Timer hardware abstraction layer

The `HW_TIM` driver is the generic, reusable hw-layer abstraction over the
MCU's timer peripherals. It presents a counter, output-compare, and trigger
API addressed by logical channel, usable by any consumer through a single
target-independent header.

Each channel maps to one physical timer peripheral (`HW_TIM_CHANNEL_*`) and
carries a counter with a configured direction, period, and tick rate; zero or
more output-compare units, each driving an output line from a compare value in
raw counts; and an optional trigger output and break input.

See also: [[overview]] (sys~arch_005~1).

## Driver configuration and lifecycle

### Timer initialization and configuration validation
`fw~hal_tim_001~1`

The timer HW driver shall validate the supplied configuration before use and
initialize every configured timer channel, starting its counter with its
outputs in their configured inactive state, returning false if the
configuration is rejected by any of:

| Element | Rejected when |
| ------- | ------------- |
| Config | The config pointer or its channel array is NULL, or the channel count exceeds the available timer peripherals. |
| Counter | The configured period or prescaler exceeds the channel's counter width, or the count direction is unsupported by the channel. |
| Output-compare unit | A unit's compare value exceeds the configured period, or a complementary unit's dead-time exceeds the channel's supported range. |

Acceptance:
- A valid config initializes every channel, leaves each counter advancing and
  each output inactive, and returns true.
- Each rejection condition returns false.

Covers:
- sys~arch_005~1

Needs: impl, test

## Counter time base

### Counter direction and period
`fw~hal_tim_002~1`

Each timer channel's counter shall advance in its configured direction (up,
down, or center-aligned) over its configured period at a tick rate of the
peripheral clock divided by the configured prescaler.

Acceptance:
- A channel configured up counts from zero to its period then rolls over; a
  channel configured down counts from its period to zero; a channel configured
  center-aligned counts up to its period then back down.
- The counter advances one step per prescaled clock tick.

Covers:
- sys~arch_005~1

Needs: impl, test

### Free-running counter readout
`fw~hal_tim_003~1`

The driver shall return the present counter value of an addressed channel in
raw counts; a read of an uninitialized driver or an out-of-range channel
returns false.

Acceptance:
- A read after a known number of elapsed ticks returns the corresponding
  counter value.
- A read of an out-of-range channel or before initialization returns false.

Covers:
- sys~arch_005~1

Needs: impl, test

## Output-compare and PWM

### Output-compare unit operation
`fw~hal_tim_004~1`

Each output-compare unit shall drive its output line from a runtime-settable
compare value in raw counts under its configured output mode, and shall be
individually enabled or disabled, driving its configured inactive level while
disabled.

Acceptance:
- Setting a unit's compare value changes its output waveform accordingly, and
  the value reads back.
- A disabled unit drives its configured inactive level regardless of compare
  value; enabling it resumes output.
- Setting a compare value above the configured period returns false.

Covers:
- sys~arch_005~1

Needs: impl, test

### Complementary outputs with dead-time
`fw~hal_tim_005~1`

An output-compare unit configured for complementary output shall drive its
paired primary and complementary lines in antiphase, with the configured
dead-time inserted before each line activates.

Acceptance:
- A complementary unit's two output lines are antiphase.
- The configured dead-time separates one line's deactivation from its
  complement's activation, in both switching directions.

Covers:
- sys~arch_005~1

Needs: impl, test

## Synchronization and fault handling

### Trigger output for peripheral synchronization
`fw~hal_tim_006~1`

A timer channel shall emit a trigger output on its configured source event,
either the counter update or an output-compare match.

Acceptance:
- A channel configured to trigger on its update event emits a trigger at each
  period boundary.
- A channel configured to trigger on an output-compare match emits a trigger
  when the counter reaches that unit's compare value.

Covers:
- sys~arch_005~1

Needs: impl, test

### Break input safe-state shutdown
`fw~hal_tim_007~1`

On a channel with a break input configured, an asserted break signal shall
force that channel's outputs to their configured inactive state and hold them
there while the break remains asserted.

Acceptance:
- Asserting the break input drives every output of the channel to its inactive
  level.
- The outputs remain inactive for as long as the break stays asserted.

Covers:
- sys~arch_005~1

Needs: impl, test
