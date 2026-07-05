---
status: draft
tags: [firmware, mc]
---

# Gate driver (dev_gateDriver)

Dev-layer driver for the STSPIN32G4's integrated gate driver, addressed as an
IO_i2c device (fixed 7-bit address 0x47, 8-bit register offsets) and accessing
registers per `fw~io_i2c_002~1`. Registers referenced below (8-bit): POWMNG
0x01, LOGIC 0x02, READY 0x07, NFAULT 0x08, CLEAR 0x09, LOCK 0x0B, STATUS 0x80.

### Initialization and config validation
`fw~mc_001~1`

On initialization the dev_gateDriver driver shall set every channel to the
unconfigured state with zeroed status, returning false if the configuration
is rejected by any of:

| Element | Rejected when |
|---------|---------------|
| Config  | The config pointer or its channel array is NULL, or the channel count is not the compiled channel count. |
| Channel | Its IO_i2c device is out of range. |

Acceptance:
- A valid configuration initializes every channel to the unconfigured state
  with zeroed status and returns true.
- Each rejection condition returns false.

Covers:
- sys~mc_002~1

Needs: impl, test

### Configuration sequence
`fw~mc_002~1`

On a fetch of an unconfigured channel, the dev_gateDriver driver shall bring
the channel's device to its configured state as a sequence — unlock the
protected registers by writing a LOCK value whose LOCK field equals the
bitwise NOT of its NLOCK field, write the channel's configured POWMNG, LOGIC,
READY, and NFAULT register values, verify each written register by readback,
relock by writing a LOCK value violating that condition, and clear the
latched faults (`fw~mc_005~1`) — marking the channel configured when every
step succeeds and leaving it unconfigured when any step fails.

Acceptance:
- After a fetch in which every write and readback succeeds, the channel
  reports configured and each configuration register read back equals the
  configured value.
- The unlock write precedes the protected-register writes and the relock
  write follows them (e.g. unlock 0x0F, relock 0x00).
- A fetch in which any write or readback fails leaves the channel
  unconfigured, and a subsequent fetch repeats the sequence.

Covers:
- sys~mc_002~1

Needs: impl, test

### Status fetch
`fw~mc_003~1`

On a fetch of a configured channel, the dev_gateDriver driver shall read the
STATUS register into the channel's cached status, recording whether the read
succeeded; a failed read leaves the previously cached flags unchanged.

Acceptance:
- After a fetch with STATUS reading 0x0D, the cached status reports reset,
  VDS protection, and VCC undervoltage set and thermal shutdown and lock
  clear.
- A fetch in which the STATUS read fails leaves the cached flags unchanged
  and records the failure.

Covers:
- sys~mc_003~1

Needs: impl, test

### Cached-status accessors
`fw~mc_004~1`

The dev_gateDriver driver shall return a channel's cached state through an
accessor for the configured state and a snapshot accessor copying the raw
STATUS byte, its decoded flags (lock, reset, VDS protection, thermal
shutdown, VCC undervoltage), and whether the most recent STATUS read
succeeded; a read of an out-of-range channel or of a channel before
successful initialization returns false (a zeroed snapshot).

Acceptance:
- Each accessor returns the value stored by the channel's most recent fetch.
- A read of an out-of-range channel or before initialization returns false
  (snapshot accessor: returns false and zeroes the caller's snapshot).

Covers:
- sys~mc_003~1

Needs: impl, test

### Clear latched faults
`fw~mc_005~1`

On a clear-faults request for a valid channel after successful
initialization, the dev_gateDriver driver shall write 0xFF to the CLEAR
register of the channel's device and return the write's success; a request
for an out-of-range channel or before successful initialization returns
false.

Acceptance:
- A clear-faults request writes 0xFF to register 0x09 of the channel's device
  and returns true when the write succeeds.
- A request for an out-of-range channel or before initialization returns
  false with no register write.

Covers:
- sys~mc_003~1

Needs: impl, test
