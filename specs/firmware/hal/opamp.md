---
status: draft
tags: [firmware, hal, opamp, driver]
---

# OPAMP hardware abstraction layer

The `HW_OPAMP` driver is the generic, reusable hw-layer abstraction over the
MCU's internal operational amplifiers. It presents an init-only API addressed
by logical channel: each channel amplifies the analog voltage at a configured
input pin by a configured gain onto the amplifier's internal ADC input.
Channel configuration (amplifier instance, input pin, gain) is supplied per
project.

See also: [[overview]] (sys~arch_005~1), [[adc]] (HW_ADC samples the
amplifier outputs).

## Driver configuration and lifecycle

### Initialization and configuration validation
`fw~hal_opamp_001~1`

The driver shall validate the supplied configuration and initialize every
configured channel — each amplifier calibrated for input offset and running
with its configured input and gain, its output driving the amplifier's
internal ADC input — returning false on any of:

| Element | Rejected when |
|---------|---------------|
| Config  | The config pointer or its channel array is NULL, or the channel count exceeds the available amplifiers. |
| Channel | Its amplifier's calibration or start fails. |

Acceptance:
- A valid configuration calibrates and starts every channel and returns true.
- Each rejection condition returns false.

Covers:
- sys~arch_005~1

Needs: impl, test

## Amplification

### PGA amplification onto the internal ADC path
`fw~hal_opamp_002~1`

Each initialized channel shall drive its amplifier's internal ADC input with
the voltage at the channel's configured input pin multiplied by the channel's
configured gain.

Acceptance:
- A conversion of the amplifier's internal ADC input, addressed per
  fw~hal_adc_002~1, reads the channel's input voltage multiplied by its
  configured gain.

Covers:
- sys~arch_005~1

Needs: impl, test
