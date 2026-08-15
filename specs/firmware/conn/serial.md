---
status: draft
tags: [firmware, conn, serial, driver]
---

# Serial transport layer

The `IO_serial` driver is the io-layer abstraction over a byte-stream serial
transport. It presents a transmit/receive API addressed by logical channel,
usable by any consumer through a single target-independent header; each channel
is backed by one transport — the USB CDC interface today.

See also: [[overview]] (sys~arch_003~1), [[usb]] (HW_USB provides the CDC
transport).

## Driver configuration and lifecycle

### Serial initialization and configuration validation
`fw~conn_serial_001~1`

The driver shall validate the supplied configuration before use and initialize
every configured channel, returning false if the configuration is rejected by
any of:

| Element | Rejected when |
|---------|---------------|
| Config | The config pointer or its channel array is NULL, or the channel count exceeds the available serial channels. |
| Channel | A channel names an unavailable backing transport. |

Acceptance:
- A valid config initializes every channel and returns true.
- Each rejection condition returns false.

Covers:
- sys~arch_003~1

Needs: impl, test

### Logical channel addressing over a backing transport
`fw~conn_serial_002~1`

The driver shall address each serial stream by logical channel, where each
channel is backed by one transport, taking only a channel identifier from the
caller.

Acceptance:
- A transmit or receive call identifies its stream by channel alone, with no
  transport argument.
- The USB-CDC-backed channel transmits and receives through this API.

Covers:
- sys~arch_003~1

Needs: impl, test

## Data transfer

### Byte transmission with bounded backpressure
`fw~conn_serial_003~1`

On a channel, the driver shall transmit caller bytes to the backing transport,
yielding while the transport is full and dropping a byte that stays unaccepted
past a bounded retry limit.

Acceptance:
- Bytes the transport accepts are transmitted in order.
- A byte a persistently-full transport never accepts is dropped and the call
  returns without blocking indefinitely.

Covers:
- sys~arch_003~1

Needs: impl, test

### Byte reception
`fw~conn_serial_004~1`

On a channel, the driver shall report the number of bytes available from the
backing transport and read them into a caller buffer.

Acceptance:
- With bytes available on a channel, the count reflects them and a read returns
  up to that many bytes in order.
- With nothing available, the count is zero and a read returns no bytes.

Covers:
- sys~arch_003~1

Needs: impl, test

### Channel connection status
`fw~conn_serial_005~1`

The driver shall report whether a channel's backing transport is connected.

Acceptance:
- A channel whose transport has an open host connection reads connected.
- A channel whose transport has no open connection reads not connected.

Covers:
- sys~arch_003~1

Needs: impl, test

### Free transmit capacity
`fw~conn_serial_006~1`

On a channel, the driver shall report the free transmit capacity of the
backing transport: the number of bytes a transmit call accepts without
yielding.

Acceptance:
- With the transport's transmit buffer empty, the reported capacity is
  the buffer's full size.
- With N undrained bytes written, the reported capacity is N lower.

Covers:
- sys~arch_003~1

Needs: impl, test
