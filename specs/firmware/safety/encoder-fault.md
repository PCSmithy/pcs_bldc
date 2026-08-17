---
status: draft
tags: [firmware, safety]
---

# Encoder-fault trip

The motor-control application watches the rotor encoder's per-read integrity
and latches a fault when it fails persistently, since six-step commutation is
driven entirely by the encoder angle.

See also: [[overcurrent]] (fw~safety_001~1, the other in-module trip),
[[motor-control-application]] (fw~mc_007~1 carries the clear gesture),
[[encoder]] (IO_AS5048 supplies the per-read status).

### Encoder-fault trip
`fw~safety_002~1`

Each control cycle the application shall read the rotor encoder's integrity
status (IO_AS5048) and count consecutive invalid reads; above five consecutive
invalid reads it shall command the bridge disabled through the enable gate and
latch a fault, released only by the fault-clear action (the button hold
of fw~mc_007~1 or the host command of fw~conn_server_004~1). A valid
read resets the count.

Acceptance:
- More than five consecutive invalid encoder reads disable the bridge and latch
  the fault.
- Five or fewer invalid reads, or a valid read within a run, do not latch.
- The latch persists once reads recover; the fault-clear action releases it.

Covers:
- sys~safety_002~1

Needs: impl, test
