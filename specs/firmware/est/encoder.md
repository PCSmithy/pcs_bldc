---
status: draft
tags: [firmware, est, encoder, driver]
---

# AS5048 magnetic encoder driver

The `IO_AS5048` driver is the io-layer abstraction over the AS5048 magnetic
rotary encoder. It presents an angle-readout API addressed by logical channel,
usable by any consumer through a single target-independent header, and obtains
each reading over the SPI HW layer.

Each channel is one AS5048 device, addressed by logical channel
(`IO_AS5048_CHANNEL_*`) and mapped to one HW_SPI channel; multiple encoders may
share a single SPI bus through distinct chip-selects.

See also: [[motor-control]] (sys~mc_001~1), [[spi]] (HW_SPI provides the
transport).

## Driver configuration and lifecycle

### Encoder initialization and configuration validation
`fw~est_encoder_001~1`

The driver shall validate the supplied configuration before use and initialize
every configured channel, returning false if the configuration is rejected by
any of:

| Element | Rejected when |
|---------|---------------|
| Config | The config pointer or its channel array is NULL, or the channel count exceeds the available encoder channels. |
| Channel | A channel maps to an out-of-range SPI channel. |

Acceptance:
- A valid config initializes every channel and returns true.
- Each rejection condition returns false.

Covers:
- sys~mc_001~1

Needs: impl, test

### Logical channel addressing over a shared SPI bus
`fw~est_encoder_002~1`

The driver shall address each encoder by logical channel, where each channel
maps to one SPI channel, and multiple channels may share a single SPI bus.

Acceptance:
- Two channels mapped to distinct SPI channels on one bus are sampled and read
  independently.
- The readout API takes a channel identifier and requires no SPI channel or
  chip-select argument from the caller.

Covers:
- sys~mc_001~1

Needs: impl, test

## Sampling and readout

### Polled angle sampling
`fw~est_encoder_003~1`

Each sampling pass shall read every configured channel's current rotor angle
and update that channel's stored angle.

Acceptance:
- After a sampling pass, each channel's stored angle reflects its most recent
  reading.
- A sampling pass invoked before initialization stores nothing.

Covers:
- sys~mc_001~1

Needs: impl, test

### Angle readout in counts and degrees
`fw~est_encoder_004~1`

The driver shall return an addressed channel's latest stored angle as a raw
14-bit count and as degrees

$$\theta = \frac{c}{16384}\, 360^\circ$$

where $c$ is the raw count; a read of an uninitialized driver or an
out-of-range channel returns false.

Acceptance:
- For a known count, the degrees reading equals the formula and the count
  reading equals the raw count.
- A read before initialization or of an out-of-range channel returns false.

Covers:
- sys~mc_001~1

Needs: impl, test

### Per-channel rotation direction
`fw~est_encoder_005~1`

A channel configured reverse shall report its angle in the opposite mechanical
sense as $(360^\circ - \theta)\bmod 360^\circ$, where $\theta$ is that channel's
forward angle of `fw~est_encoder_004~1`.

Acceptance:
- For one physical position, a reverse-configured channel reports
  $(360^\circ - \theta)\bmod 360^\circ$ where $\theta$ is the forward reading.
- A channel not configured reverse reports $\theta$ unchanged.

Covers:
- sys~mc_001~1

Needs: impl, test

## Transaction integrity

### Frame integrity validation and fault status
`fw~est_encoder_006~1`

Each per-channel read shall validate its SPI response frame's even parity and
error flag; a passing read updates the channel's stored angle and clears its
fault status, and a failing read leaves the stored angle unchanged and sets its
fault status.

Acceptance:
- A response with valid parity and a clear error flag updates the stored angle
  and clears the channel's fault status.
- A response with a parity mismatch or a set error flag leaves the stored angle
  unchanged and sets the channel's fault status.
- The fault status is readable per channel.

Covers:
- sys~mc_001~1

Needs: impl, test
