---
status: draft
tags: [firmware, mc, app, driver]
---

# Field-oriented control methods

Current control on the encoder: PI regulators on the rotor-frame currents
set a rotor-frame voltage, applied through the inverse Park transform at
the per-cycle rotor angle and the modulator. The torque method takes its
demand from the torque target (fw~mc_026~1); the velocity method from a PI
regulator on the rotor velocity estimate; either, less the cogging
compensation (fw~mc_028~1), sets the q-axis current target.

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

i_d* = 0, i_q* = T_q / K_t

with all three phases enabled, for T_q the q-axis torque command
(fw~mc_028~1) formed from the torque target T_t (fw~mc_026~1) and K_t the
channel's configured torque constant in Nm/A.

Acceptance:
- With I_d and I_q held at zero, the cogging amplitudes zero
  (fw~mc_028~1) and K_p · I_max below V_lim, over a sweep of torque
  targets through ±K_t · I_max the method's first commutation step after a
  bridge enable commands v_q = K_p · T_t / K_t and v_d = 0 (fw~mc_023~1),
  within 1 % of K_p · I_max.
- The stator voltage vector recovered from the three duties equals the
  limited (v_d, v_q) rotated by the rotor electrical angle, within 2 % of
  V_max, with every phase enabled.
- A configured maximum current at or above the 1.2 A phase trip
  (fw~safety_001~1) is rejected at init.

Covers:
- sys~mc_001~1
- sys~mc_005~1

Needs: impl, test

### Torque target
`fw~mc_026~1`

The application shall hold a torque target in Nm as the request state the
field-oriented torque method drives (fw~mc_022~1), written by each torque
setpoint directly and by each speed target while the torque method is
selected as sgn(ω*) · min(|ω*| / ω_max, 1) · K_t · I_max with the speed
target's rate limit scaled by K_t · I_max / ω_max (fw~mc_027~1) — the most
recent write standing (sys~ops_002~1), a bridge disable or a latched fault
clearing it — and saturated to ±K_t · I_max, for ω_max the channel's
configured maximum velocity, K_t its configured torque constant, and I_max
its configured maximum current.

Acceptance:
- A torque setpoint of 0.4 Nm followed by a speed target of ω_max / 2
  with the torque method selected leaves the target at K_t · I_max / 2;
  the reverse order leaves it at 0.4 Nm.
- A torque setpoint of 2 · K_t · I_max leaves the target at K_t · I_max.
- A speed target with another method selected leaves the torque target
  unchanged.
- A speed target of ω_max / 2 with a limit of ω_max / 2 rad/s² and the
  torque method selected ramps the torque target from zero to
  K_t · I_max / 2 over 1 s.
- A torque setpoint of 0.4 Nm, then a disable or a latched fault and a
  re-enable with no further write, leaves the target at zero.

Covers:
- sys~mc_001~1
- sys~mc_005~1
- sys~ops_002~1

Needs: impl, test

### Field-oriented velocity commutation
`fw~mc_025~1`

Each commutation step while active, the field-oriented velocity method
shall command, as the torque method does (fw~mc_022~1), the
voltage-limited (fw~mc_024~1) output of the current regulator
(fw~mc_023~1) with all three phases enabled for the targets i_d* = 0 and
i_q* = T_q / K_t, T_q the q-axis torque command (fw~mc_028~1) formed from
the method's demand

T* = K_vp · e + K_vi · S_v − k_ff · T̂_d, then S_v ← S_v + T · e, for e = ω* − ω̂

a T* of magnitude at or above K_t · I_max then clamped to ±K_t · I_max
with S_v set so that K_vp · e + K_vi · S_v − k_ff · T̂_d equals the clamped
T*, and S_v zero at the method's first commutation step after each bridge
enable and each method selection, for ω* the speed target and ω̂ the
per-cycle rotor velocity estimate, both in mechanical rad/s, T̂_d the
per-cycle unmodelled-torque estimate in Nm (fw~est_rotor_002~1), T the
PWM period, I_max the channel's configured maximum current, K_t its
configured torque constant in Nm/A, K_vp and K_vi its configured velocity
gains in Nm per rad/s and Nm per rad, and k_ff its configured disturbance
feed-forward gain in [0, 1].

Acceptance:
- With ω̂ and T̂_d held at zero, the cogging amplitudes zero (fw~mc_028~1)
  and ω* stepped to Ω at a bridge enable, with K_vp · Ω + 20 · K_vi · T · Ω
  below K_t · I_max, the method's first commutation step sets
  i_q* = K_vp · Ω / K_t and each of the next 20 steps K_vi · T · Ω / K_t
  more, within 1 % of K_vp · Ω / K_t.
- With ω̂ and T̂_d held at zero, the cogging amplitudes zero and ω* stepped
  to Ω with K_vp · Ω between K_t · I_max and 2 · K_t · I_max, every step
  sets i_q* = I_max, and the step after ω* returns to zero sets
  i_q* = I_max − K_vp · Ω / K_t, each within 1 % of I_max.
- With ω̂ and ω* zero, the cogging amplitudes zero, k_ff = 0.5 and T̂_d
  held at D with |D| below K_t · I_max, the method's first commutation step
  after a bridge enable sets i_q* = −0.5 · D / K_t within 1 %.
- With ω̂ and ω* zero, the cogging amplitudes zero, k_ff = 1 and T̂_d held
  at −2 · K_t · I_max, the method's first commutation step after a bridge
  enable sets i_q* = I_max, and with T̂_d then held at zero the next step
  sets i_q* = −I_max, each within 1 % of I_max.
- At K_vp = 0.04 Nm per rad/s, K_vi = 0.5 Nm per rad, k_ff = 0, the
  cogging amplitudes zero and K_t · I_max above 0.4 Nm, on a free rotor of
  1.6e-4 kg·m² tracked by an estimator configured for it
  (fw~est_rotor_001~1, fw~est_rotor_002~1), a step of ω* from rest to
  10 rad/s brings ω̂ to a peak under 11 rad/s and within 5 % of ω* from
  100 ms to 600 ms after the step.
- At K_vp = 0.04 Nm per rad/s, K_vi = 0.5 Nm per rad, k_ff = 0 and the
  cogging amplitudes zero, with the rotor held at rest and ω* at 10 rad/s,
  i_q* reaches I_max inside 2 s and holds there over the following 500 ms,
  and the step after ω* returns to zero sets i_q* = I_max − K_vp · 10 rad/s
  / K_t, within 1 % of I_max.
- On the rotor and estimator above at the same K_vp and K_vi with the
  cogging amplitudes zero, with the rotor turning steadily at ω* = 10 rad/s
  under a load torque of 9 mNm amplitude varying sinusoidally at 2.3 Hz,
  the amplitude of ω̂'s ripple over 1 s from 500 ms after the load begins
  is, at k_ff = 1, under a quarter of its value at k_ff = 0.
- A configuration with a torque constant or a velocity gain at or below
  zero, or a disturbance feed-forward gain below 0 or above 1, is rejected
  at init.

Covers:
- sys~mc_001~1
- sys~mc_005~1

Needs: impl, test

### Cogging-torque compensation
`fw~mc_028~1`

Each commutation step for any field-oriented method, the application
shall form the q-axis torque command T_q, from which i_q* = T_q / K_t
(fw~mc_022~1, fw~mc_025~1), as

T_q = T_m − k_cog · T_cog(θ̂), clamped to ±K_t · I_max, with T_cog(θ̂) = A_1 · sin(n_1 · θ̂ + φ_1) + A_2 · sin(n_2 · θ̂ + φ_2)

for T_m the method's demand (the torque target, fw~mc_026~1, under the
torque method; the clamped T* under the velocity method, fw~mc_025~1),
θ̂ the estimator's mechanical angle for the cycle in rad
(fw~est_rotor_002~1), T_cog the configured model of the cogging torque on
the rotor in Nm, positive in the direction of increasing θ̂, with n_1 and
n_2 its configured harmonic orders in cycles per mechanical revolution,
A_1 and A_2 their configured amplitudes in Nm at or above zero and φ_1
and φ_2 their configured phases in rad, k_cog the configured compensation
gain in [0, 1], K_t the channel's configured torque constant in Nm/A and
I_max its configured maximum current.

Acceptance:
- For any field-oriented method with I_d, I_q, ω̂, ω*, T̂_d, k_ff and the
  torque target zero, K_p · I_max below V_lim, A_1 = 0.2 Nm, n_1 = 28,
  φ_1 = 0, A_2 = 0 and k_cog = 0.5, the method's first commutation step
  after a bridge enable commands v_q = −K_p · 0.1 Nm / K_t at a rotor
  angle where sin(28 · θ̂) = 1 and v_q = 0 where it is 0, each within 1 %
  of K_p · 0.1 Nm / K_t, with v_d = 0 (fw~mc_023~1).
- Under the same conditions with A_1 = 0, A_2 = 0.2 Nm, n_2 = 56,
  φ_2 = π/2 and k_cog = 1, the first step at θ̂ = 0 commands
  v_q = −K_p · 0.2 Nm / K_t within 1 % of K_p · 0.2 Nm / K_t.
- Under the torque method with I_d and I_q zero, K_p · I_max below V_lim,
  a torque target of K_t · I_max, k_cog = 1 and T_cog(θ̂) = −0.2 Nm at the
  rotor angle, the first step commands v_q = K_p · I_max within 1 % of
  K_p · I_max.
- At K_vp = 0.04 Nm per rad/s, K_vi = 0.5 Nm per rad, k_ff = 0 and
  K_t · I_max above 0.4 Nm, on a free rotor of 1.6e-4 kg·m² tracked by an
  estimator configured for it (fw~est_rotor_001~1, fw~est_rotor_002~1)
  and carrying a cogging torque of 10 mNm · sin(28 · θ), with A_1 = 10 mNm,
  n_1 = 28, φ_1 = 0 and A_2 = 0 configured, under the velocity method
  holding ω* = 2 rad/s, the amplitude of the rotor velocity's 28-per-turn
  ripple over 1 s from 500 ms after the hold begins is, at k_cog = 1,
  under a fifth of its value at k_cog = 0.
- A configuration with a compensation gain below 0 or above 1, or an
  amplitude below zero, is rejected at init.

Covers:
- sys~mc_001~1

Needs: impl, test

### Rotor-frame current regulator
`fw~mc_023~1`

Each commutation step for any field-oriented method, the current
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

Each commutation step for any field-oriented method, the current
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
