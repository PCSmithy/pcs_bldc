---
status: draft
tags: [firmware, hal, adc, driver]
---

# ADC hardware abstraction layer

The `HW_ADC` driver is the generic, reusable hw-layer abstraction over the
MCU's ADC peripherals. It presents a sampled-conversion API addressed by
logical channel and input, usable by any consumer through a single
target-independent header.

The configuration is split in two:

- **Channels** enumerate the physical ADC peripherals (`HW_ADC_CHANNEL_*`).
- **Inputs** enumerate the analog inputs on a channel. Each channel carries a
  regular conversion sequence (inputs indexed by physical input number) and an
  injected conversion sequence (inputs indexed by dense sequence position).

See also: [[overview]] (sys~arch_005~1).

## Driver configuration and lifecycle

### ADC initialization and configuration validation
`fw~hal_adc_001~1`

The ADC HW driver shall validate the supplied configuration before use and
initialize every configured channel, calibrating each, returning false if the
configuration is rejected by any of:

| Element        | Rejected when                                                                                                                                        |
| -------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------- |
| Config         | The config pointer or its channel array is NULL, or the channel count exceeds the available ADC channels.                                            |
| Regular input  | A channel's enabled regular inputs' conversion ranks do not form the contiguous sequence 1..N, where N is the channel's enabled regular-input count. |
| Injected input | A channel's enabled injected inputs are not contiguous from the first sequence position.                                                             |

Acceptance:
- A valid config initializes and calibrates every channel and returns true.
- Each rejection condition returns false.

Covers:
- sys~arch_005~1

Needs: impl, test

### Regular-sequence channel and input addressing
`fw~hal_adc_002~1`

The driver shall address each regular conversion by logical channel and
physical input number, converting a channel's enabled regular inputs in
ascending configured-rank order.

Acceptance:
- A channel with N enabled regular inputs converts exactly those N inputs,
  ordered by ascending rank; disabled inputs are not converted.
- Enabling input number k on a channel converts that channel's input k.

Covers:
- sys~arch_005~1

Needs: impl, test

## Conversion triggering and transfer

### ADC trigger and transfer modes
`fw~hal_adc_003~1`

Each ADC channel's regular and injected sequences shall each operate under one
trigger mode and one transfer mode:

| Trigger mode | Sequence start |
|--------------|----------------|
| **Software** | The CPU starts the conversion sequence on demand. |
| **Timer** | A hardware timer event starts the conversion sequence. |

| Transfer mode | Result extraction |
|---------------|-------------------|
| **Polled** | The CPU waits for each conversion and reads its result. |
| **Interrupt** | Conversions complete under interrupt and results are read in the handler. |
| **DMA** | A DMA stream moves results and signals completion asynchronously. |

Acceptance:
- A channel configured in each listed trigger and transfer mode initializes
  and produces conversions for its enabled inputs.

Covers:
- sys~arch_005~1

Needs: impl, test

### Polled software-triggered sampling
`fw~hal_adc_004~1`

On a channel configured software-triggered and polled, each sampling pass shall
start the regular sequence and, holding the sequencer after each conversion
until that conversion's result is read so no rank's result is lost or
overwritten regardless of poll latency, poll each conversion to completion
within a 2 ms per-conversion bound and store the latest raw count for every
enabled regular input; a conversion that exceeds the bound records a fault in
the channel's pollable conversion status and retains the input's prior count.

Acceptance:
- After a sampling pass, every enabled regular input's stored count reflects
  its most recent conversion.
- A multi-rank sequence stores each rank's own result, with no rank lost or
  overwritten when the poll loop runs slower than the conversions.
- A sampling pass invoked before initialization leaves all stored counts
  unchanged.
- A conversion that stalls past the 2 ms bound sets the channel's conversion
  status to fault and leaves that input's prior count unchanged.

Covers:
- sys~arch_005~1

Needs: impl, test

### Conversion result readout in counts and volts
`fw~hal_adc_005~1`

The driver shall return the latest regular conversion result for an addressed
(channel, input) as a raw count and as a voltage:

$$V = \frac{c}{2^{n} - 1}\, V_{ref}$$

where $c$ is the raw count, $n$ the channel's configured ADC resolution in
bits, and $V_{ref}$ the channel's reference voltage; a read of an uninitialized
driver, an out-of-range channel or input, a disabled input, or a NULL
destination returns false.

Acceptance:
- For a known count, the volts reading equals the formula above.
- A read of a disabled input, an out-of-range index, or before initialization
  returns false.

Covers:
- sys~arch_005~1

Needs: impl, test

### Asynchronous conversion completion model
`fw~hal_adc_008~1`

For sequences configured for a non-blocking transfer mode (interrupt or DMA),
the driver shall report conversion-group completion through both a per-channel
completion callback and a pollable per-channel conversion status.

Acceptance:
- A non-blocking conversion group's per-channel status transitions
  busy → complete (or → error), and a registered completion callback is
  invoked exactly once at completion.
- Completion is observable by polling alone (no callback) and by callback
  alone, with consistent results.

Covers:
- sys~arch_005~1

Needs: impl, test

## Injected and multi-ADC operation

### Injected conversion sequence
`fw~hal_adc_006~1`

Each channel shall provide an injected conversion sequence whose enabled inputs
are addressed by dense sequence position, sampled per the channel's injected
trigger and transfer mode (`fw~hal_adc_003~1`) and read back per position as a
raw count and a voltage by the formula of `fw~hal_adc_005~1`.

Rationale:
- The injected group is hardware-prioritized — an injected trigger preempts the
  regular sequence — and lands results in four dedicated registers, so inputs
  use a dense 0..3 position. The driver carries it, with timer triggering
  (`fw~hal_adc_003~1`) and dual-ADC multimode (`fw~hal_adc_007~1`), for
  field-oriented control: phase currents sampled simultaneously across two ADCs
  at the PWM-period center, hardware-triggered from the TIM1 update event.

Acceptance:
- A channel with M enabled injected inputs makes all M positions readable after
  sampling.
- An injected read of a disabled position, an out-of-range position, or before
  initialization returns false.

Covers:
- sys~arch_005~1

Needs: impl, test

### Dual-ADC multimode configuration
`fw~hal_adc_007~1`

For a channel flagged as a multimode master, the driver shall apply that
channel's multimode configuration to its ADC pair during initialization.

Acceptance:
- A master channel's configured multimode is applied during init; a channel
  not flagged as master applies no multimode configuration.

Covers:
- sys~arch_005~1

Needs: impl, test
