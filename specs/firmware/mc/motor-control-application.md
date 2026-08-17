---
status: draft
tags: [firmware, mc, app, driver]
---

# Motor-control application

The `app_motorControl` module owns the drive loop: each control cycle it
runs one active commutation method — selected from a registered set — and
applies the method's outputs to the bridge through `IO_bridge`. The operator
interface is the user button (gestures), the dial encoder (speed target),
and the LED ring (state indication).

See also: [[commutation-method-architecture]] (sys~mc_005~1),
[[bridge]] (fw~io_bridge_002/003, the duty and enable path),
[[gate-driver]] (dev_gateDriver supplies the operational gate),
[[overcurrent]] (fw~safety_001~1, the trip that force-disables).

## Dispatch and gating

### Active-method dispatch and bridge gating
`fw~mc_006~1`

The application shall gate the drive loop each 1 ms control cycle: while
the bridge is enabled it invokes the active commutation method with the
speed target, motor shaft angle, and phase currents, applying the method's
per-phase duty and output-enable commands through IO_bridge; an enable
request takes effect only with dev_gateDriver_isOperational true and no
fault latched, the bridge otherwise held disabled. Engagement follows the
enable (run/stop) state, not the instantaneous speed target: while enabled
and the active method aligned, the bridge master output enable stays
asserted through a zero target — the phases idle at zero duty — and only a
disable request or a latched fault deasserts it.

Acceptance:
- While enabled, the active method's commands reach IO_bridge every cycle.
- An enable request with the gate driver not operational, or with a fault
  latched, leaves the bridge disabled.
- While disabled, no method runs and the bridge output enable stays
  deasserted.
- Engagement follows the enable (run/stop) state, not the instantaneous
  speed target.
- A zero speed target while enabled and aligned holds the master output
  enable asserted, the phases idling at zero duty; zero demand alone never
  deasserts it.

Covers:
- sys~mc_005~1

Needs: impl, test

## Operator interface

### Button gesture mapping
`fw~mc_007~1`

The application shall map user-button gestures to control actions by
context:

| Context | Gesture | Action |
|---------|---------|--------|
| Bridge disabled, no fault | Press shorter than 1 s | Advance the active method to the next registered method |
| Bridge disabled, no fault | Hold of 1 s or longer | Enable the bridge |
| Bridge enabled | Any press | Disable the bridge |
| Fault latched | Hold of 3 s or longer | Clear the fault latch |

Acceptance:
- Each table row's gesture in its context produces its action.
- A gesture outside its context produces no action (a hold of 1 s or
  longer while faulted does not enable the bridge; a short press while
  faulted neither cycles the method nor clears the fault).

Covers:
- sys~mc_005~1

Needs: impl, test

### Dial speed target
`fw~mc_008~1`

The application shall maintain the dial's speed demand, writing each
change of it to the shared speed target (`sys~ops_002~1`): the demand is
zero at each bridge enable it commands, adjusted by dial-encoder angle
deltas scaled by a configured gain, and clamped to a configured maximum
magnitude.

Acceptance:
- At each on-device-commanded bridge enable the dial demand restarts at
  zero.
- Dial motion adjusts the demand by the configured gain per degree,
  signed by direction.
- The demand magnitude never exceeds the configured maximum.
- A demand change, including the enable-time zero, appears as the shared
  speed target.

Covers:
- sys~mc_005~1

Needs: impl, test

### Ring state indication
`fw~mc_009~1`

The application shall present the active method identity and the bridge
state — disabled, enabled, or faulted — on the LED ring, with a change
reflected in the next 10 ms ring frame.

Acceptance:
- Each registered method has a distinct ring presentation.
- The three bridge states are distinguishable on the ring.
- A method or state change appears in the next ring frame.

Covers:
- sys~mc_005~1

Needs: impl, test
