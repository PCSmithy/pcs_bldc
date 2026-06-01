---
status: draft
tags: [firmware, hal, spi, driver]
---

# SPI hardware abstraction layer

The `HW_SPI` driver is the generic, reusable hw-layer abstraction over the
MCU's SPI peripherals. It presents a byte-transfer API addressed by logical
channel, usable by any consumer through a single target-independent header.

The configuration is split in two:

- **Buses** enumerate the physical SPI peripherals available on the MCU
  (`HW_SPI_BUS_*`).
- **Channels** enumerate the logical devices (`HW_SPI_CHANNEL_*`), each
  mapped to one bus, so multiple channels may share a bus.

See also: [[overview]] (sys~arch_005~1).

## Driver configuration and lifecycle

### SPI initialization and configuration validation
`fw~hal_spi_001~1`

The SPI HW driver shall validate the supplied configuration before use and
shall initialize every enabled SPI bus, returning false if the configuration
is rejected by any of:

| Element | Rejected when |
|---------|---------------|
| Config | The config pointer is NULL. |
| Bus | An enabled bus specifies a transfer mode the target does not support. |
| Channel | It references a disabled or out-of-range bus, or its chip-select configuration is invalid per `fw~hal_spi_007~1`. |

Acceptance:
- A valid config initializes all enabled buses and returns true.
- Each rejection condition above returns false.

Covers:
- sys~arch_005~1

Needs: impl, test

### Logical channel addressing over shared buses
`fw~hal_spi_002~1`

The SPI HW driver shall address all transfers by logical channel, where each
channel maps to exactly one physical bus and one independent chip-select, and
multiple channels may share a single bus.

Acceptance:
- Two channels on the same bus with distinct chip-selects each transact
  independently; a transfer asserts only the addressed channel's
  chip-select.
- The public transfer API takes a channel identifier and requires no bus or
  chip-select argument from the caller.

Covers:
- sys~arch_005~1

Needs: impl, test

### Chip-select configuration validity
`fw~hal_spi_007~1`

A channel's chip-select configuration shall be valid only when its mode and
the fields that mode requires are well-formed:

| CS mode | Valid when |
|---------|-----------|
| **GPIO** | The port is in range and the pin is a single valid GPIO line. |
| **Hardware** | Always (no additional fields). |
| **None** | Always (no additional fields). |

Acceptance:
- A GPIO config with an out-of-range port is invalid.
- A GPIO config with a pin that is not exactly one valid GPIO line is
  invalid.
- A GPIO config with an in-range port and a single valid pin is valid.
- A hardware-mode and a none-mode config are valid regardless of GPIO
  fields.

Covers:
- sys~arch_005~1

Needs: impl, test

### Driver-managed chip-select with configurable polarity
`fw~hal_spi_004~1`

The driver shall manage each channel's chip-select per its configured mode:

| CS mode | Driver behavior |
|---------|-----------------|
| **GPIO** | Asserts the configured chip-select GPIO at its configured active polarity for the duration of a transfer, then deasserts it. |
| **Hardware** | Relies on the peripheral's native NSS; drives no GPIO. |
| **None** | Drives no chip-select. |

Acceptance:
- A GPIO active-low channel drives CS low immediately before the transfer
  and high immediately after; a GPIO active-high channel drives the inverse.
- A hardware-NSS channel toggles no GPIO.
- A none channel transfers with no chip-select activity.

Covers:
- sys~arch_005~1

Needs: impl, test

## Transfer modes

### SPI transfer modes
`fw~hal_spi_006~1`

Each SPI bus shall operate in one of the following transfer modes:

| Mode | Bus behavior |
|------|--------------|
| **Software** | The CPU drives the transfer and blocks until it completes or times out. |
| **Interrupt** | The transfer proceeds under peripheral interrupts; the call returns immediately and completion is signalled asynchronously. |
| **DMA** | A DMA stream moves the data; the call returns immediately and completion is signalled asynchronously. |

Acceptance:
- A bus configured in each listed mode completes a transfer successfully.

Covers:
- sys~arch_005~1

Needs: impl, test

### Blocking byte transfers with computed timeout
`fw~hal_spi_003~1`

On a bus configured for software (polled) transfer mode, the driver shall
perform blocking byte-oriented transmit, receive, and transmit-receive
transfers of caller-specified length, enforcing a per-transfer timeout

$$t_{timeout} = \left\lceil\, 1.1 \cdot \frac{8N}{f_{bit}} + 1\,\text{ms} \,\right\rceil$$

where $N$ is the transfer length in bytes and $f_{bit}$ is the bus bit rate
(peripheral clock ÷ baud-rate prescaler), rounded up to whole milliseconds.

Acceptance:
- transmit / receive / transmitReceive move exactly `length` bytes for the
  addressed channel and return true on success.
- A transfer that does not complete within $t_{timeout}$ returns false
  rather than blocking indefinitely.

Covers:
- sys~arch_005~1

Needs: impl, test

### Asynchronous transfer completion model
`fw~hal_spi_005~1`

For buses configured for a non-blocking transfer mode (interrupt or DMA), the
driver shall report transfer completion through both a per-channel completion
callback and a pollable per-channel transfer status.

Acceptance:
- A non-blocking transfer's per-channel status transitions busy → complete
  (or → error), and a registered completion callback is invoked exactly once
  at completion.
- Completion is observable by polling alone (no callback) and by callback
  alone, with consistent results.

Covers:
- sys~arch_005~1

Needs: impl, test
