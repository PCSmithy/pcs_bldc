---
status: draft
tags: [firmware, safety]
---

# Overcurrent trip

The software overcurrent trip is the standing protection for all bridge
operation: a 1 kHz threshold check over the phase and VBUS current sense
channels that force-disables the bridge and latches.

See also: [[motor-control-application]] (fw~mc_007~1 carries the clear
gesture), [[overcurrent]] (sys~safety_001~1, the system anchor).

### Software overcurrent trip
`fw~safety_001~1`

At each 1 ms control cycle the firmware shall compare every phase current
magnitude against 2 A and the VBUS current against 1.5 A, and on any
exceedance command the bridge output disabled through IO_bridge and latch a
fault released only by the fault-clear action (the button hold of fw~mc_007~1
or the host command of fw~conn_server_004~1).

Acceptance:
- A phase current magnitude above 2 A on any phase disables the bridge and
  latches the fault within one control cycle.
- A VBUS current above 1.5 A disables the bridge and latches the fault
  within one control cycle.
- The latch persists with currents below threshold; the fault-clear action
  releases it.

Covers:
- sys~safety_001~1

Needs: impl, test
