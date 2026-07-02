---
status: draft
tags: [firmware, hal, dma, driver]
---

# DMA hardware abstraction layer

The `HW_DMA` driver is the generic, reusable hw-layer abstraction over the MCU's
DMA controller. It presents a single-shot memory↔peripheral transfer engine
addressed by logical channel, usable by any consumer through a single
target-independent header. Each channel is configured for a fixed transfer
direction, data width, and peripheral request; a consumer starts a transfer and
learns of its completion without blocking.

See also: [[overview]] (sys~arch_005~1), [[spi]] (HW_SPI consumes HW_DMA for
non-blocking transfers).

## Driver configuration and lifecycle

### Initialization and configuration validation
`fw~hal_dma_001~1`

The driver shall validate the supplied configuration and initialize every
configured channel, returning false if the configuration is rejected by any of:

| Element | Rejected when |
|---------|---------------|
| Config | The config pointer or its channel array is NULL, or the channel count exceeds the available DMA channels. |
| Channel | Its transfer direction or data width is not a supported value, or it maps to an unavailable DMA resource. |

Acceptance:
- A valid configuration initializes every channel and returns true.
- Each rejection condition returns false.

Covers:
- sys~arch_005~1

Needs: impl, test

## Transfer initiation

### Single-shot memory–peripheral transfer
`fw~hal_dma_002~1`

The driver shall start a non-blocking transfer of a requested number of data
items on a channel, moving them in the channel's configured direction and data
width:

| Direction | Transfer |
|-----------|----------|
| Memory to peripheral | Delivers the memory buffer's items to the peripheral, in order. |
| Peripheral to memory | Fills the memory buffer from the peripheral, in order. |

Acceptance:
- Each configured direction moves the requested items as described.
- A start request on an out-of-range or uninitialized channel, with a null
  buffer, or with a zero item count, returns false and starts no transfer.

Covers:
- sys~arch_005~1

Needs: impl, test

## Completion and error reporting

### Asynchronous transfer completion
`fw~hal_dma_003~1`

The driver shall report each transfer's outcome through both a per-channel
completion callback and a pollable per-channel status: the status reads busy
from a transfer's start until it reads complete on success or error on failure,
and a registered callback is invoked exactly once per transfer.

Acceptance:
- A started transfer reports busy until it completes.
- A successful transfer's status becomes complete; a failed transfer's becomes
  error.
- A registered completion callback is invoked exactly once per transfer.
- Completion is observable by polling alone and by callback alone, with
  consistent results.

Covers:
- sys~arch_005~1

Needs: impl, test
