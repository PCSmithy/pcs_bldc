---
status: draft
tags: [firmware, hal, usb, driver]
---

# USB hardware abstraction layer

The `HW_USB` driver is the hal-layer abstraction over the MCU's USB device
peripheral. It presents a CDC virtual-serial byte interface — host-connection
state, byte transmit, and byte receive — usable by any consumer through a
single target-independent header.

See also: [[overview]] (sys~arch_005~1), [[serial]] (IO_serial layers a
channelized byte stream on this interface).

## Driver configuration and lifecycle

### USB CDC interface bring-up
`fw~hal_usb_001~1`

The driver shall bring up its CDC virtual-serial interface, returning false if
the device stack cannot be started.

Acceptance:
- A successful bring-up returns true and the CDC interface can service host
  connection, transmit, and receive operations.
- A bring-up that cannot start the device stack returns false.

Covers:
- sys~arch_005~1

Needs: impl, test

## Connection and data transfer

### Host connection state
`fw~hal_usb_002~1`

The driver shall report whether a host has opened the CDC virtual-serial port.

Acceptance:
- With a host port open, the connection state reads connected.
- With no host port open, the connection state reads not connected.

Covers:
- sys~arch_005~1

Needs: impl, test

### CDC byte transmission
`fw~hal_usb_003~1`

The driver shall accept caller bytes for transmission to the host, reporting how
many were accepted.

Acceptance:
- A write into available transmit space reports every byte accepted.
- A write larger than the available transmit space reports only the bytes that
  fit, and reports zero when the transmit space is full.

Covers:
- sys~arch_005~1

Needs: impl, test

### CDC free transmit capacity
`fw~hal_usb_005~1`

The driver shall report the number of bytes a transmit write
(`fw~hal_usb_003~1`) accepts in full.

Acceptance:
- With the transmit space empty, the reported capacity is the full
  transmit buffer size, and a write of that size reports every byte
  accepted.
- With N accepted, undrained bytes, the reported capacity is N lower.

Covers:
- sys~arch_005~1

Needs: impl, test

### CDC byte reception
`fw~hal_usb_004~1`

The driver shall report the number of bytes received from the host and read them
into a caller buffer.

Acceptance:
- With bytes received, the available count reflects them and a read returns up
  to that many bytes in order.
- With no bytes received, the available count is zero and a read returns no
  bytes.

Covers:
- sys~arch_005~1

Needs: impl, test
