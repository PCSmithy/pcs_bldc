---
status: draft
tags: [firmware, io, i2c, driver]
---

# I2C device driver

The `IO_i2c` driver is the generic io-layer abstraction over I2C devices. It
presents a register read/write API addressed by logical device, usable by any
consumer through a single target-independent header, and performs each access
over the I2C HW layer.

A device is addressed by logical identifier (`IO_I2C_DEVICE_*`) and mapped in
configuration to one HW_I2C bus, a 7-bit device address, and a register-offset
width; multiple devices may share a single bus.

See also: [[overview]] (sys~arch_005~1), [[hal/i2c]] (HW_I2C provides the bus
transport).

## Driver configuration and lifecycle

### Device initialization and configuration validation
`fw~io_i2c_001~1`

The IO_i2c driver shall validate the supplied configuration before use and
initialize every configured device, returning false if the configuration is
rejected by any of:

| Element | Rejected when |
|---------|---------------|
| Config | The config pointer or its device array is NULL, or the device count exceeds the available devices. |
| Device | A device maps to an out-of-range HW_I2C bus. |

Acceptance:
- A valid config initializes every device and returns true.
- Each rejection condition returns false.

Covers:
- sys~arch_005~1

Needs: impl, test

### Logical device addressing
`fw~io_i2c_002~1`

The IO_i2c driver shall address each access by logical device, supplying that
device's HW_I2C bus and 7-bit address from configuration, and multiple devices
may share a single bus.

Acceptance:
- An access to a device is issued on the bus and 7-bit address configured for
  that device, selected solely by the device identifier.
- Two devices mapped to one bus at distinct addresses are each accessed
  independently.

Covers:
- sys~arch_005~1

Needs: impl, test

## Access

### Register read and write
`fw~io_i2c_003~1`

The IO_i2c driver shall perform register reads and register writes to a logical
device, issuing each as a HW_I2C register transfer that uses the device's
configured register-offset width.

Acceptance:
- A register read of a device returns the bytes at the addressed register, using
  that device's configured offset width.
- A register write of a device stores the caller's bytes at the addressed
  register, using that device's configured offset width.

Covers:
- sys~arch_005~1

Needs: impl, test
