---
status: draft
tags: [firmware, hal, crc, driver]
---

# CRC hardware abstraction layer

The `HW_CRC` driver is the hw-layer abstraction over the MCU's CRC unit,
fixed to the IEEE 802.3 CRC-32 the protocol frames carry
([[serial]], fw~conn_proto). It serves one user at a time, the protocol
server's frame path. The sim computes the same values in software.

See also: [[overview]] (sys~arch_005~1), [[control-loop-timing]]
(sys~mc_006~1, the budget the unit serves).

## Driver configuration and lifecycle

### Initialization and self-check
`fw~hal_crc_001~1`

The driver shall configure the unit for the IEEE 802.3 CRC-32, prove it
on the standard check value, and return false when the config pointer is
NULL or the check value is not reproduced.

Acceptance:
- A valid configuration returns true, and the computation over the bytes
  "123456789" then reads 0xCBF43926.
- A NULL config returns false.

Covers:
- sys~arch_005~1

Needs: impl, test

## Computation

### CRC-32 of a byte sequence
`fw~hal_crc_002~1`

After initialization, the driver shall return the IEEE 802.3 CRC-32 of a
byte sequence of any length up to a frame's maximum, zero for an empty
sequence, and zero before initialization.

Acceptance:
- The bytes "123456789" read 0xCBF43926 and an empty sequence reads 0.
- Repeated computations over the same bytes agree, for lengths across the
  frame range.

Covers:
- sys~arch_005~1

Needs: impl, test
