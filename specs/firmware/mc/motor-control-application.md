---
status: draft
tags: [firmware, mc, app, driver]
---

# Motor-control application

The `app_motorControl` module owns the drive loop across two cadences. The
1 ms cycle owns the inputs and the gate: it reads the encoder and publishes
the rotor angle and velocity estimate, the speed target, mode selection,
bus voltage, fault latching, alignment, and bridge enable. The
PWM-synchronous cycle, entered from the bridge's per-cycle callback that
`main.c` composes around the duration probe (`fw~mc_018~1`), runs the active
commutation method's step on the per-cycle rotor angle, those inputs, and
the cycle's phase currents, and writes the bridge commands. The operator
interface is the user button (gestures), the dial encoder (speed target),
and the LED ring (state indication).

See also: [[commutation-method-architecture]] (`sys~mc_005~1`), [[bridge]]
(`fw~io_bridge_002/003/004/008` the duty and enable path, `fw~io_bridge_007~1`
the per-cycle callback), [[modulation]] (`fw~mc_013~1`, the rotor-frame
transform), [[velocity]] (`fw~est_velocity_001~1`, the velocity estimate),
[[six-step]], [[sinusoidal]], [[vf-sinusoidal]] (the commutation methods),
[[gate-driver]] (dev_gateDriver supplies the operational gate),
[[overcurrent]] (`fw~safety_001~1`, the trip that force-disables).

## Dispatch and gating

### Bridge enable gating
`fw~mc_006~1`

The application shall hold the bridge enabled exactly while an enable
request stands with dev_gateDriver_isOperational true, no fault latched,
and the precharge (`fw~mc_021~1`), the alignment pattern (`fw~mc_012~1`),
or the active method driving at least one phase, and otherwise disabled
with zero duties.

Acceptance:
- An enable request with the gate driver not operational, or with a fault
  latched, leaves the bridge disabled.
- While disabled, the bridge output enable stays deasserted and the duties
  are zero.
- A zero speed target while enabled holds the master output enable
  asserted.
- A fault latched while enabled deasserts the master output enable.

Covers:
- `sys~mc_005~1`

Needs: impl, test

### PWM-synchronous commutation step
`fw~mc_015~1`

On each bridge cycle callback (`fw~io_bridge_007~1`) while the bridge is
enabled with the alignment offset captured (`fw~mc_012~1`), the application
shall run the active method's commutation step with the speed target and
bus voltage as last published by the 1 ms cycle, the per-cycle rotor
electrical angle (`fw~mc_019~1`), and the cycle's phase currents, and apply
the step's bridge commands — per-phase duties (`fw~io_bridge_002~1`) or a
voltage vector (`fw~io_bridge_008~1`), and per-phase output enables
(`fw~io_bridge_004~1`) — through IO_bridge within the same PWM period.

Acceptance:
- Over N PWM periods while enabled, the active method's step runs N times
  and each step's commands are on the phase outputs in the following
  period.
- A speed-target or bus-voltage change published by the 1 ms cycle is used
  by the next step.

Covers:
- `sys~mc_005~1`

Needs: impl, test

### Rotor-frame current observation
`fw~mc_016~1`

Each commutation step, the application shall transform the cycle's phase
currents into the rotor frame (`fw~mc_013~1`) at the active method's frame
angle and hold the result as the drive's I_d and I_q:

| Method                        | Frame angle                                      |
|-------------------------------|--------------------------------------------------|
| Six-step, sensored sinusoidal | Per-cycle rotor electrical angle (`fw~mc_019~1`) |
| V/f                           | Commanded electrical angle (`fw~mc_010~1`)       |

Acceptance:
- With the frame angle at 0 and phase currents (I, −I/2, −I/2), I_d = I and
  I_q = 0; with the frame angle at 90°, I_d = 0 and I_q = −I.
- At constant speed with the phase currents rotating at the electrical
  frequency, I_d and I_q hold within 5% of their magnitude.

Covers:
- `sys~mc_001~1`

Needs: impl, test

### Cycle callback duration probe
`fw~mc_018~1`

The firmware shall hold, as readable statics, the durations in
microseconds, measured on the bridge's free-running microsecond time
base, from bridge cycle callback entry to the end of the commutation
step and from entry to callback exit: the latest callback's, and the
maximum observed, each maximum cleared by writing zero to it
(`fw~conn_trace_008~1`).

Acceptance:

- After each callback, the latest-duration statics equal that
  callback's durations, within 1 µs.
- After N callbacks, each maximum equals the largest of the N
  corresponding durations, within 1 µs.
- Writing zero to a maximum restarts it from the next callback.

Covers:
- `sys~mc_006~1`

Needs: impl, test

## Rotor angle

### Per-cycle rotor electrical angle
`fw~mc_019~1`

Each PWM cycle, the application shall hold the rotor electrical angle —
the shaft angle less the alignment offset (fw~mc_012~1), times the
pole-pair count, as sampled by the 1 ms cycle — advanced by the electrical
velocity, the pole-pair count times the velocity estimate
(fw~est_velocity_001~1), over the time since that sample, wrapped to
[0, 2π).

Acceptance:
- At a constant 300 rad/s electrical, every PWM cycle's angle is within 3°
  of the rotor's electrical angle.

Covers:
- `sys~mc_005~1`

Needs: impl, test

### Bootstrap precharge on enable
`fw~mc_021~1`

On every transition from the bridge disabled to enabled, the application
shall hold all three phases enabled at zero duty for the configured
precharge time before the alignment pattern (`fw~mc_012~1`) or the active
method drives, so the gate driver's bootstrap supplies charge through the
low sides before a high side is commanded on.

Rationale:
- A dark bridge's bootstrap capacitors drain; a high side commanded on
  from an empty one leaves its MOSFET off, and the gate driver's VDS
  protection latches after its deglitch time (6 µs on this board), which
  a 50 % zero vector exceeds on the first cycle.

Acceptance:
- From the first millisecond of an enable until the precharge time has
  elapsed, the three duties are zero with every phase enabled and the
  master output enable asserted; the alignment pattern or the method
  follows only then.
- An enable after a disable precharges again, then drives without
  re-aligning.
- A configuration with a zero precharge time is rejected at init.

Covers:
- `sys~mc_005~1`

Needs: impl, test

### Alignment offset capture
`fw~mc_012~1`

On the first bridge enable after power-up, the application shall drive the
phase-U axis pattern — U at the configured alignment duty, V and W
returning at zero duty — for the configured dwell time, then capture the
motor shaft angle as the alignment offset: the electrical zero on which
every subsequent enable commutates.

Acceptance:
- The first enable applies the alignment pattern for the dwell time, then
  captures the offset, then the active method commutates.
- Subsequent enables commutate immediately with the stored offset.
- A zero speed target during commutation leaves the stored offset in use.

Covers:
- `sys~mc_005~1`

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
- `sys~mc_005~1`

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
- `sys~mc_005~1`

Needs: impl, test

### Ring state indication
`fw~mc_009~1`

The application shall present the motor-control mode and the drive
state — disabled, enabled, or faulted — on the LED ring, with a change
reflected in the next 10 ms ring frame.

Acceptance:
- Each registered method has a distinct ring presentation.
- The three drive states are distinguishable on the ring.
- A mode or drive-state change appears in the next ring frame.

Covers:
- `sys~mc_005~1`

Needs: impl, test
