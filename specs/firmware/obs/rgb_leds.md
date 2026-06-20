---
status: draft
tags: [firmware, obs, rgb_led, driver]
---

# SK6805 RGB LED string driver

The `IO_SK6805` driver is the io-layer abstraction over SK6805 RGB LED strings.
It presents a colour-control API addressed by logical channel and pixel index,
usable by any consumer through a single target-independent header, and drives
each string over the SPI HW layer.

Each channel is one SK6805 LED string, addressed by logical channel
(`IO_SK6805_CHANNEL_*`) and mapped to one HW_SPI channel; multiple strings may
share a single SPI bus through distinct chip-selects.

See also: [[overview]] (sys~arch_001~1), [[spi]] (HW_SPI provides the transport).

## Driver configuration and lifecycle

### LED-string initialization and configuration validation
`fw~obs_led_001~1`

The driver shall validate the supplied configuration before use and initialize
every configured channel, returning false if the configuration is rejected by
any of:

| Element | Rejected when |
|---------|---------------|
| Config | The config pointer or its channel array is NULL, or the channel count exceeds the available LED-string channels. |
| Channel | A channel maps to an out-of-range SPI channel. |

Acceptance:
- A valid config initializes every channel and returns true.
- Each rejection condition returns false.

Covers:
- sys~arch_001~1

Needs: impl, test

### Logical channel addressing over a shared SPI bus
`fw~obs_led_002~1`

The driver shall address each LED string by logical channel, where each channel
maps to one SPI channel, and multiple channels may share a single SPI bus.

Acceptance:
- Two channels mapped to distinct SPI channels are written and transmitted
  independently.
- The colour-control API takes a channel identifier and requires no SPI channel
  or chip-select argument from the caller.

Covers:
- sys~arch_001~1

Needs: impl, test

## Colour control and transmission

### Per-pixel framebuffer
`fw~obs_led_003~1`

Each channel shall expose a per-pixel framebuffer of 8-bit-per-channel RGB
colour, with operations to set a single pixel by index, set every pixel at
once, and clear the string to off; a write to an out-of-range pixel index is
ignored.

Acceptance:
- Setting a pixel, then setting it again, leaves the most recent colour in the
  framebuffer.
- Setting every pixel applies one colour across the string; clearing sets every
  pixel to off.
- A write to an out-of-range pixel index leaves the framebuffer unchanged.

Covers:
- sys~arch_001~1

Needs: impl, test

### Frame transmission
`fw~obs_led_004~1`

The driver shall transmit a channel's framebuffer to its LED string as an
SK6805-protocol frame, returning false if the driver is uninitialized or the
transfer fails.

Acceptance:
- A transmit emits each pixel's stored colour, unmodified, in framebuffer order.
- A transmit before initialization, or one whose SPI transfer fails, returns
  false.

Covers:
- sys~arch_001~1

Needs: impl, test

## Output polarity

### Inverting-output polarity
`fw~obs_led_005~1`

A channel configured invert shall transmit with the output signal polarity
inverted.

Acceptance:
- On a channel configured invert, the entire transmitted signal is the bitwise
  complement of the non-inverting transmission.
- A channel not configured invert transmits the signal directly.

Covers:
- sys~arch_001~1

Needs: impl, test
