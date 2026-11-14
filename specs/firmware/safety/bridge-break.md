---
status: draft
tags: [firmware, safety]
---

# Bridge break latch

The gate driver's fault line feeds the PWM timer's break input, which clears
the bridge's master output enable in hardware. The motor-control application
turns that hardware disable into a latched fault.

See also: [[overcurrent]] (fw~safety_001~1, the latch and its clear action),
[[bridge]] (fw~io_bridge_003~1 reports the output enable, a break included).

### Bridge break latch
`fw~safety_003~1`

The application shall write the bridge output enable only when its commanded
state changes, and on each 1 ms cycle in which it has commanded the enable
asserted and the bridge reports it deasserted (fw~io_bridge_003~1), withdraw
the enable request and latch a fault, released only by the fault-clear action
(the button hold of fw~mc_007~1 or the host command of fw~conn_server_004~1).

Acceptance:
- A break that clears the output enable under a driving bridge leaves it
  deasserted on every following cycle and latches the fault within 1 ms.
- The output enable reading deasserted before the first cycle of an enable
  latches nothing.
- The fault-clear action releases the latch, and the bridge stays disabled
  until a new enable request.

Covers:
- sys~mc_004~1
- sys~safety_001~1

Needs: impl, test
