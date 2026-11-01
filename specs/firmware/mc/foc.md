---
status: draft
tags: [firmware, mc, app, driver]
---

# Field-oriented control methods

Current control on the encoder: PI regulators on the rotor-frame currents
set a rotor-frame voltage, applied through the inverse Park transform at
the per-cycle rotor angle and the modulator. The torque method takes its
q-axis current target from the speed target; the velocity method takes it
from the torque demand of a PI regulator on the rotor velocity estimate.

See also: [[motor-control-application]] (fw~mc_015~1 runs each method's
step each PWM cycle, fw~mc_016~1 supplies I_d and I_q, fw~mc_019~1 the
per-cycle rotor angle), [[rotor]] (fw~est_rotor_002~1, the per-cycle
velocity estimate), [[modulation]] (fw~mc_013~1 is the inverse Park
transform, fw~mc_014~1 the modulator), [[bridge]] (the voltage vector is
applied by the bridge, fw~io_bridge_008~1; fw~io_bridge_009~1 the
offset-corrected phase currents), [[commutation-method-architecture]]
(sys~mc_005~1), [[motor-control]] (sys~mc_001~1).

### Field-oriented torque commutation
`fw~mc_022~1`

Each commutation step while active, the field-oriented torque method shall
command, through the inverse Park transform (fw~mc_013~1) at the per-cycle
rotor electrical angle (fw~mc_019~1) and the bridge voltage-vector command
(fw~io_bridge_008~1), the voltage-limited (fw~mc_024~1) output of the
current regulator (fw~mc_023~1) for the cycle's I_d and I_q (fw~mc_016~1)
and the targets

i_d* = 0, i_q* = sgn(ω*) · min(|ω*| / ω_max, 1) · I_max

with all three phases enabled, for ω* the speed target, ω_max the channel's
configured maximum mechanical velocity, and I_max its configured maximum
current.

Acceptance:
- With I_d and I_q held at zero and K_p · I_max below V_lim, over a sweep
  of speed targets through ±ω_max the method's first commutation step
  after a bridge enable commands v_q = K_p · i_q* and v_d = 0
  (fw~mc_023~1), within 1 % of K_p · I_max.
- The stator voltage vector recovered from the three duties equals the
  limited (v_d, v_q) rotated by the rotor electrical angle, within 2 % of
  V_max, with every phase enabled.
- A configured maximum current at or above the 1.2 A phase trip
  (fw~safety_001~1) is rejected at init.

Covers:
- sys~mc_001~1
- sys~mc_005~1

Needs: impl, test

### Field-oriented velocity commutation
`fw~mc_025~1`

Each commutation step while active, the field-oriented velocity method
shall command, as the torque method does (fw~mc_022~1), the
voltage-limited (fw~mc_024~1) output of the current regulator
(fw~mc_023~1) with all three phases enabled for the targets i_d* = 0 and
i_q* = T* / K_t, with the torque demand

T* = K_vp · e + K_vi · S_v, then S_v ← S_v + T · e, for e = ω* − ω̂

a T* of magnitude at or above K_t · I_max then clamped to ±K_t · I_max
with S_v set so that K_vp · e + K_vi · S_v equals the clamped T*, and S_v
zero at the method's first commutation step after each bridge enable and
each method selection, for ω* the speed target and ω̂ the per-cycle rotor
velocity estimate (fw~est_rotor_002~1), both in mechanical rad/s, T the
PWM period, I_max the channel's configured maximum current, K_t its
configured torque constant in Nm/A, and K_vp and K_vi its configured
velocity gains in Nm per rad/s and Nm per rad.

Acceptance:
- With ω̂ held at zero and ω* stepped to Ω at a bridge enable, with
  K_vp · Ω + 20 · K_vi · T · Ω below K_t · I_max, the method's first
  commutation step sets i_q* = K_vp · Ω / K_t and each of the next 20
  steps K_vi · T · Ω / K_t more, within 1 % of K_vp · Ω / K_t.
- With ω̂ held at zero and ω* stepped to Ω with K_vp · Ω between K_t · I_max
  and 2 · K_t · I_max, every step sets i_q* = I_max, and the step after ω*
  returns to zero sets i_q* = I_max − K_vp · Ω / K_t, each within 1 % of
  I_max.
- At K_vp = J · ω_v and K_vi = K_vp · ω_v / 10 with ω_v = 50 rad/s, for J
  the inertia of the driven rotor, a step of ω* from rest to 10 rad/s with
  the rotor free brings ω̂ to 63 % of the step inside 2 / ω_v, overshooting
  by under 10 % of the step, and within 2 % of ω* from 500 ms after the
  step on.
- At the same gains with the rotor held at rest and ω* at 10 rad/s, i_q*
  reaches I_max inside 2 s and holds there, and the step after ω* returns
  to zero sets i_q* = I_max − K_vp · 10 rad/s / K_t, within 1 % of I_max.
- A configuration with a torque constant or a velocity gain at or below
  zero is rejected at init.

Covers:
- sys~mc_001~1
- sys~mc_005~1

Needs: impl, test

### Rotor-frame current regulator
`fw~mc_023~1`

Each commutation step under either field-oriented method, the current
regulator shall produce the rotor-frame voltage command (v_d, v_q) from the
current errors

e_x = i_x* − I_x, x ∈ {d, q}

as

v_x = K_p · e_x + K_i · S_x, then S_x ← S_x + T · e_x

with S_d and S_q zero at the method's first commutation step after each
bridge enable and each method selection, where

| Symbol | Value |
|--------|-------|
| T      | the PWM period |
| K_p    | L · ω_bw |
| K_i    | R · ω_bw |

for L and R the channel's configured phase inductance and resistance and
ω_bw its configured current-loop bandwidth in rad/s.

Acceptance:
- With I_d and I_q held at zero and i_q* stepped to I at a bridge enable,
  with K_p · I + 20 · K_i · T · I below V_lim, the method's first
  commutation step commands v_q = K_p · I and each of the next 20 steps
  K_i · T · I more, v_d staying zero, within 1 % of K_p · I.
- Driving the configured R and L with the rotor held at rest, a step in
  i_q* with K_p · step below V_lim brings I_q to 63 % of the step inside
  2 / ω_bw, overshooting by under 10 % of the step.
- A configuration with a phase resistance, phase inductance, or bandwidth
  of zero, or with ω_bw · T above 0.2, is rejected at init.

Covers:
- sys~mc_001~1

Needs: impl, test

### Voltage limit with anti-windup
`fw~mc_024~1`

Each commutation step under either field-oriented method, the current
regulator shall limit its voltage command to the magnitude

V_lim = min(V_max, V_bus / √3)

scaling a command of magnitude at or above V_lim to V_lim at its own angle
and then setting each integral term S_x so that K_p · e_x + K_i · S_x
equals the limited v_x, for V_max the channel's configured maximum voltage
and V_bus the bus voltage as published by the 1 ms cycle (fw~mc_015~1).

Acceptance:
- With I_d and I_q held at zero and i_q* stepped to I with K_p · I between
  V_lim and 2 · V_lim, every step commands magnitude V_lim on the q axis,
  and the step after i_q* returns to zero commands V_lim − K_p · I, each
  within 1 % of V_lim.
- Driving the configured R with the rotor held at rest and i_q* · R above
  V_lim, from 10 ms after i_q* is applied the vector magnitude recovered
  from the three duties at the published V_bus is within 2 % of V_lim.

Covers:
- sys~mc_001~1

Needs: impl, test
