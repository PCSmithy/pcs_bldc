---
status: draft
tags: [firmware, mc, app, driver]
---

# Motor-control application

The `app_motorControl` module owns the drive loop across two cadences. The
1 ms cycle owns the inputs and the gate: the speed, torque and position
targets and their limits, mode selection,
bus voltage, fault latching, alignment, and bridge enable. The
PWM-synchronous cycle, entered from the bridge's per-cycle callback that
`main.c` composes around the duration probe (`fw~mc_018~1`), runs the active
commutation method's step on the per-cycle rotor electrical angle
(`fw~mc_019~1`) from the rotor estimate ([[rotor]]), those inputs, and the
cycle's phase currents, and writes the bridge commands. The operator
interface is the user button (gestures), the dial encoder (speed target),
and the LED ring (state indication).

See also: [[commutation-method-architecture]] (`sys~mc_005~1`), [[bridge]]
(`fw~io_bridge_002/003/004/008` the duty and enable path, `fw~io_bridge_007~1`
the per-cycle callback, `fw~io_bridge_009~1` the offset-corrected phase
currents), [[modulation]] (`fw~mc_013~1`, the rotor-frame
transform), [[rotor]] (`fw~est_rotor_002~1`, the angle and velocity
estimates), [[six-step]], [[sinusoidal]], [[foc]], [[vf-sinusoidal]] (the
commutation methods),
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

### Method selection
`fw~mc_033~1`

The application shall take each mode request as follows, the selected method
starting as the channel's configured default:

| Request | While the active method drives | Otherwise |
|---|---|---|
| A method | The active method's own: none; another: ignored | Selects the method and requests an enable |
| OFF | Withdraws the enable request, the selection kept | Withdraws the enable request, the selection kept |

Acceptance:
- Another method requested while six-step drives leaves six-step selected
  and commutating.
- OFF then disables the bridge, and the other method's request selects it.
- After OFF, the selected method is the one last requested.

Covers:
- `sys~mc_005~1`

Needs: impl, test

### PWM-synchronous commutation step
`fw~mc_015~1`

On each cycle callback (`fw~io_bridge_007~1`) of a motor-control channel's
bridge while that bridge is enabled with the alignment offset captured (`fw~mc_012~1`), the application
shall run the active method's commutation step with the speed and torque
targets, the position target and profile velocity (`fw~mc_031~1`) and bus
voltage as last published by the 1 ms cycle, the per-cycle rotor
electrical angle (`fw~mc_019~1`) and mechanical velocity estimate
(`fw~est_rotor_002~1`), and the cycle's phase currents, and apply the
step's bridge commands — per-phase duties (`fw~io_bridge_002~1`) or a
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

### Cycle callback duration probe
`fw~mc_018~1`

The firmware shall hold, as readable statics, the durations in
microseconds, measured on the bridge's free-running microsecond time
base, from bridge cycle callback entry to the end of the commutation
step — the rotor estimate advance (`fw~est_rotor_002~1`) included — and
from entry to callback exit: the latest callback's, and the
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

## Phase currents

### Rotor-frame current observation
`fw~mc_016~1`

Each bridge cycle callback (`fw~io_bridge_007~1`), the application shall
transform the bridge-reported phase currents (`fw~io_bridge_009~1`) into
the rotor frame (`fw~mc_013~1`) at the active method's frame angle, hold
the result as the drive's I_d and I_q, and set the estimator's torque
input (`fw~est_rotor_002~1`) to K_t · I_q + T_k for the method's known
cogging share T_k, for K_t the motor-control channel's configured torque
constant in Nm/A and θ̂ the estimator's mechanical angle for the cycle:

| Method                             | Frame angle                                      | Known cogging share T_k           |
|------------------------------------|--------------------------------------------------|-----------------------------------|
| Field-oriented torque, velocity and position | Per-cycle rotor electrical angle (`fw~mc_019~1`) | k_cog · T_cog(θ̂) (`fw~mc_028~1`) |
| Six-step, sensored sinusoidal      | Per-cycle rotor electrical angle (`fw~mc_019~1`) | 0                                 |
| V/f                                | Commanded electrical angle (`fw~mc_010~1`)       | 0                                 |
| Any, before the first alignment offset capture (`fw~mc_012~1`) | Per-cycle rotor electrical angle (`fw~mc_019~1`) | 0, and the torque input zero |

Acceptance:
- With the frame angle at 0 and phase currents (I, −I/2, −I/2), I_d = I and
  I_q = 0; with the frame angle at 90°, I_d = 0 and I_q = −I.
- At constant speed with the phase currents rotating at the electrical
  frequency, I_d and I_q hold within 5% of their magnitude.
- Under the six-step method each callback sets the estimator's torque
  input to K_t · I_q, within 1 % of K_t · I_max (`fw~mc_022~1`).
- Before the first alignment offset capture, phase currents of
  I_q = −0.3 A set the estimator's torque input to zero.
- Under the field-oriented torque method with the rotor held at an angle
  where sin(28 · θ̂) = 1, A_1 = 0.2 Nm, n_1 = 28, φ_1 = 0, A_2 = 0 and
  k_cog = 0.5 (`fw~mc_028~1`), each callback sets the estimator's torque
  input to K_t · I_q + 0.1 Nm, within 1 % of 0.1 Nm.

Covers:
- `sys~mc_001~1`

Needs: impl, test

## Rotor angle

### Per-cycle rotor electrical angle
`fw~mc_019~1`

Each PWM cycle, the application shall hold the rotor electrical angle: the
estimator's mechanical angle for the cycle (`fw~est_rotor_002~1`) less the
alignment offset (`fw~mc_012~1`), times the pole-pair count, wrapped to
[0, 2π).

Acceptance:
- At ω_n = 2π · 50 rad/s, ζ = 1, p_d = 157 rad/s (`fw~est_rotor_001~1`),
  b and T_c configured zero (`fw~est_rotor_002~1`), and 14 pole pairs,
  after 100 ms at a constant 300 rad/s electrical, every PWM cycle's angle
  is within 3° of the rotor's electrical angle.

Covers:
- `sys~mc_005~1`

Needs: impl, test

### Rotor position
`fw~mc_032~1`

Each PWM cycle, the application shall hold the rotor position θ̂_p in
rad: zero until the active method's first commutation step after a
bridge enable, then accumulating each cycle's change in the estimator's
mechanical angle (`fw~est_rotor_002~1`) taken in (−π, π].

Acceptance:
- The estimator's angle stepped from 6.2 rad to 0.1 rad moves θ̂_p by
  +0.1832 rad; stepped from 0.1 rad to 6.2 rad, by −0.1832 rad, each
  within 1e-4 rad.
- After three turns forward and one back, θ̂_p is 4π within 1e-3 rad.
- After ten turns forward and ten back, θ̂_p is zero within 1e-3 rad.
- Through a bridge disable and the next enable's precharge, θ̂_p is
  zero.

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
estimator's mechanical angle (`fw~est_rotor_002~1`) at that tick as the
alignment offset: the electrical zero on which every subsequent enable
commutates.

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

### Setpoint rate limit
`fw~mc_027~1`

Each 1 ms cycle, the application shall move the speed target and the
torque target toward their most recently written values by at most each
target's rate limit times 1 ms — a rate limit of zero moving the target
to its written value at once — the speed target seeded from the rotor
velocity estimate (`fw~est_rotor_002~1`) and the torque target from zero
at each bridge enable, and the speed target seeded from the profile
velocity ω_f (`fw~mc_031~1`) on the cycle a speed target written under the
position method follows a position setpoint, for the rate limit in rad/s² or Nm/s written with
the target by the sequence player (`fw~ops_001~1`), and zero for a dial
speed demand (`fw~mc_008~1`).

Acceptance:
- A speed target written as 10 rad/s with a limit of 20 rad/s² from a
  standing 0 reaches 10 rad/s 500 ms later, advancing 0.02 rad/s per
  cycle within 1 %.
- A torque target written with a limit of zero is in effect the next
  cycle.
- At a bridge enable with the rotor turning at 5 rad/s, the speed target
  in effect is 5 rad/s on the first cycle; the torque target in effect is
  zero.
- A dial speed demand (`fw~mc_008~1`) is in effect the next cycle.
- Under the position method, a jog at 5 rad/s brought to rest by a
  position setpoint, then a speed target of zero with a limit of
  10 rad/s²: θ* holds.

Covers:
- `sys~ops_003~1`

Needs: impl, test

### Position setpoint
`fw~mc_030~1`

The application shall hold the position setpoint the move profile
(`fw~mc_031~1`) drives to — a position θ_s in rad with a velocity limit
v in rad/s, an acceleration limit a in rad/s² and a velocity at the
point ω_s in rad/s, written by each position setpoint of the sequence
player (`fw~ops_001~1`), the most recent write standing
(`sys~ops_002~1`), a non-finite or negative a held as zero, and none held
from each bridge disable and each latched fault until the next write.

Acceptance:
- Two position setpoints written in one pass leave the second held.
- A position setpoint written, then a bridge disable, a re-enable and
  the method's first commutation step: none is held.
- A position setpoint written while the bridge is disabled is held at
  the following enable.
- A position setpoint of 1 rad from rest with v = 10 rad/s and an infinite
  a is reached in 100 ms, within 2 ms.

Covers:
- `sys~ops_002~1`
- `sys~ops_003~1`

Needs: impl, test

### Move profile
`fw~mc_031~1`

Each 1 ms cycle from the position method's (`fw~mc_029~1`) first
commutation step after a bridge enable, the application shall advance the position target θ* by
ω_f · 1 ms, saturated to ±θ_max, and set the profile velocity ω_f by the
row below of whichever of the position setpoint (`fw~mc_030~1`) and,
while the position method (`fw~mc_029~1`) is active, the speed target
was written last — θ* and ω_f zero at the bridge enable and until that
first step, and ω_f zero while it would move θ* past a bound — and on the
n-th PWM cycle after the k-th 1 ms cycle hold the cycle's position target

θ*_c = θ*_{k−1} + (θ*_k − θ*_{k−1}) · min(n · T / 1 ms, 1)

for θ*_k the k-th cycle's θ*, T the PWM period, ω_max the channel's
configured maximum velocity and θ_max its configured maximum position in
rad, above zero:

| Written last | ω_f |
|---|---|
| The position setpoint θ_s with v, a and ω_s | The minimum-time profile to θ_s arriving at speed \|ω_s\| in the direction of approach: ω_f changing by a · 1 ms per cycle toward ±v while θ* is farther from θ_s than (ω_f² − ω_s²) / (2 · a), moving toward ω_s by a · 1 ms per cycle inside it, equal to ω_s as θ* reaches θ_s, and held at ω_s past it; a limit v of zero standing for ω_max, a limit a of zero for ω_f stepping to ±v and to ω_s |
| The speed target, a jog | The speed target in effect (`fw~mc_027~1`) |
| Neither since the enable | Zero |

Acceptance:
- From rest, θ_s = 2π rad with v = 10 rad/s, a = 50 rad/s² and ω_s = 0:
  ω_f is 10 rad/s 200 ms after the write, θ* is 2π with ω_f zero 828 ms
  after it, each within 2 ms, θ* at most 2π throughout.
- From rest, θ_s = 0.5 rad with the same limits: ω_f peaks at 5 rad/s
  100 ms after the write and θ* is 0.5 rad with ω_f zero 200 ms after
  it, each within 2 ms.
- From rest, θ_s = 2π with v = 10 rad/s, a = 0 and ω_s = 0: ω_f is
  10 rad/s on the next cycle and θ* is 2π 628 ms after the write, within
  2 ms.
- From rest, θ_s = 4π with v = 0, a = 50 rad/s² and ω_s = 0, ω_max at
  most 25 rad/s and θ_max at least 4π: ω_f reaches ω_max within 1 %
  before θ* reaches 4π.
- From rest, θ_s = 2π with v = 10 rad/s, a = 50 rad/s² and ω_s = 10 rad/s:
  θ* is 2π with ω_f 10 rad/s 728 ms after the write, within 2 ms, and ω_f
  stays 10 rad/s past it; θ_s = 4π with ω_s = 0 written at that moment is
  reached with ω_f zero 728 ms later, or 628 ms after a write 100 ms
  later, each within 2 ms.
- θ_s rewritten to 0 with v = 10 rad/s, a = 50 rad/s² and ω_s = 0 while
  ω_f is 10 rad/s toward 2π: ω_f falls 0.05 rad/s per cycle through
  zero, and θ* returns to 0 with ω_f zero.
- With the position method active, a speed target of 2 rad/s written
  with a rate limit of zero: ω_f is 2 rad/s the next cycle and θ*
  advances 0.002 rad per cycle; a speed target of zero with a rate limit
  of zero holds θ*; a position setpoint of θ* + 1 rad with v = 10 rad/s,
  a = 50 rad/s² and ω_s = 0 written from that rest is reached 283 ms
  after its write, within 2 ms.
- A jog at ω_max toward θ_max leaves θ* at θ_max with ω_f zero; a jog
  of −ω_max then moves θ* off it.
- θ_s = 1 rad with v = 10 rad/s, a = 50 rad/s² and ω_s = 0 written during
  a bridge enable's precharge: θ* is 1 rad with ω_f zero 283 ms after the
  method's first commutation step, within 2 ms.
- From rest, θ_s = 0.09 rad with v = 20 rad/s, a = 0 and ω_s = 0, twenty
  cycles to a pass, a rotor on θ*_c at ω_f every cycle draws a command of
  zero from the position method (`fw~mc_029~1`) on each cycle, θ*_c never
  past θ_s.
- Init rejects θ_max at or below zero.

Covers:
- `sys~ops_002~1`
- `sys~ops_003~1`

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
