---
status: draft
tags: [firmware, hal, i2c, driver]
---

# I2C hardware abstraction layer

The `HW_I2C` driver is the generic, reusable hw-layer abstraction over the MCU's
I2C peripherals. It presents a byte- and register-transfer API addressed by
logical bus, with the target device selected by a 7-bit address supplied per
transfer, usable by any consumer through a single target-independent header.

The configuration enumerates the physical I2C buses (`HW_I2C_BUS_*`); each bus
carries its SCL bit rate and transfer mode. Any device on a bus is reached by
supplying that device's address at transfer time.

See also: [[overview]] (sys~arch_005~1), [[io/i2c]] (IO_i2c layers a generic
device driver over this bus).

## Driver configuration and lifecycle

### I2C initialization and configuration validation
`fw~hal_i2c_001~1`

The I2C HW driver shall validate the supplied configuration before use and shall
initialize every enabled I2C bus, returning false if the configuration is
rejected by any of:

| Element | Rejected when |
|---------|---------------|
| Config | The config pointer is NULL. |
| Bus | An enabled bus specifies a transfer mode the target does not support. |

Acceptance:
- A valid config initializes all enabled buses and returns true.
- Each rejection condition above returns false.

Covers:
- sys~arch_005~1

Needs: impl, test

### Logical bus addressing with per-transfer device address
`fw~hal_i2c_002~1`

The I2C HW driver shall address each transfer by logical bus and a 7-bit device
address supplied per transfer, and multiple devices may share a single bus.

Acceptance:
- Two devices at distinct addresses on one bus each transmit and receive
  independently, selected solely by the per-transfer device address.
- The transfer API takes a bus identifier and a device-address argument, and
  requires no per-device registration.

Covers:
- sys~arch_005~1

Needs: impl, test

## Transfers

### Blocking byte transfers with computed timeout
`fw~hal_i2c_003~1`

The I2C HW driver shall perform byte-oriented transmit and receive transfers of
caller-specified length to an addressed device, suspending the calling task
until the transfer completes or a per-transfer timeout elapses

$$t_{timeout} = \left\lceil\, 1.1 \cdot \frac{9N}{f_{bit}} + 1\,\text{ms} \,\right\rceil$$

where $N$ is the number of bytes transferred and $f_{bit}$ is the bus's
configured SCL bit rate, rounded up to whole milliseconds.

Acceptance:
- transmit and receive move exactly `length` bytes for the addressed device and
  return true on success.
- A transfer that does not complete within $t_{timeout}$ returns false.
- A lower-priority ready task runs while a transfer is in progress.

Covers:
- sys~arch_005~1

Needs: impl, test

### Register read and write with configurable offset width
`fw~hal_i2c_004~1`

The I2C HW driver shall perform register-read and register-write transfers to an
addressed device as a single bus transaction, each addressing the register by an
8-bit or 16-bit offset and carrying a caller-specified number of bytes, suspending
the calling task and applying the timeout of `fw~hal_i2c_003~1`, with the
register-offset bytes counted in the transferred length.

Acceptance:
- A register read returns the addressed device's register bytes, exactly
  `length` bytes, for both an 8-bit and a 16-bit offset, and returns true on
  success.
- A register write stores `length` bytes to the addressed device's register,
  for both an 8-bit and a 16-bit offset, and returns true on success.
- A register transfer that does not complete within its timeout returns false.

Covers:
- sys~arch_005~1

Needs: impl, test
