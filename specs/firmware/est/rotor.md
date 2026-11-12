---
status: draft
tags: [firmware, est, rotor]
---

# Rotor tracking estimator

The `app_rotorEstimation` module runs a fixed-gain tracking observer, in
state-space form, on each rotor's mechanical angle, velocity, and
unmodelled torque: one estimator channel per rotor, configured with the
encoder channel it reads every millisecond, the observer's poles, the
rotor's inertia and friction, and the PWM period of the bridge whose
cycle callback carries the state forward under the torque input the
motor-control application sets. The application commutates on the
per-cycle estimate.

See also: [[encoder]] (the angle read, its direction, and its validity
are fw~est_encoder_004~1, fw~est_encoder_005~1, and fw~est_encoder_006~1),
[[motor-control-application]] (the torque input is set per fw~mc_016~1;
the per-cycle angle fw~mc_019~1 commutates on the estimate; the alignment
capture fw~mc_012~1 takes the offset from it), [[bridge]] (the per-cycle
callback, fw~io_bridge_007~1), [[status]] (the velocity publication
is fw~obs_status_001~1), [[motor-control]] (sys~mc_001~1).

### Encoder tracking correction
`fw~est_rotor_001~1`

Each 1 ms tick, the estimator shall update its state estimate of
mechanical angle, velocity, and unmodelled torque

$$\hat{x} = \begin{bmatrix} \hat\theta \\ \hat\omega \\ \hat{T}_d \end{bmatrix}$$

zero from init, from the encoder channel's read as

| Read                                                   | Update |
|--------------------------------------------------------|--------|
| The first valid one since init, or after more than $N_r$ consecutive failed or invalid ones | $\hat{x} = [\theta_m,\ 0,\ 0]^\mathsf{T}$ |
| Every later valid one                                  | $e = \operatorname{wrap}(\theta_m - H\hat{x})$ to $(-\pi, \pi]$; $\hat{x} \leftarrow \hat{x} + K e$, $\hat\theta$ wrapped to $[0, 2\pi)$ |
| A failed one, or one the driver flags invalid (fw~est_encoder_006~1) | none |

with

$$H = \begin{bmatrix} 1 & 0 & 0 \end{bmatrix}, \qquad
K = T_s \begin{bmatrix} l_1 \\ l_2 \\ l_3 \end{bmatrix}, \qquad
l_1 = 2 \zeta \omega_n + p_d - \frac{b}{J}, \quad
l_2 = \omega_n^2 + 2 \zeta \omega_n p_d - l_1 \frac{b}{J}, \quad
l_3 = J \, \omega_n^2 \, p_d$$

placing the poles of the estimation error at the roots of
(s² + 2ζω_n s + ω_n²)(s + p_d), in scalar form e = wrap(θ_m − θ̂);
T̂_d ← T̂_d + l_3 · e · T_s; ω̂ ← ω̂ + l_2 · e · T_s;
θ̂ ← wrap(θ̂ + l_1 · e · T_s), taking effect by the next PWM cycle, for
θ_m the channel's reported angle in radians
(fw~est_encoder_004~1, fw~est_encoder_005~1), T_s the 1 ms tick, ω_n the
configured natural frequency in rad/s and ζ the configured damping ratio
of the error dynamics' pole pair, p_d the configured disturbance pole in
rad/s, the error dynamics' third pole, and J and b the configured rotor
inertia in kg·m² and viscous friction in Nm per rad/s
(fw~est_rotor_002~1), and N_r the configured re-seed count.

Acceptance:
- At ω_n = 2π · 50 rad/s, ζ = 1, and p_d = 157 rad/s, with b and T_c
  configured zero and a zero torque input (fw~est_rotor_002~1), from the
  first valid read of a noise-free encoder angle advancing at a constant
  21 rad/s, ω̂ is within 1 % of the rate inside 50 ms and stays there over
  the following 1 s, through the encoder's 0/2π wraps.
- At the same configuration, from the first valid read of a noise-free
  encoder angle advancing from rest under a constant 50 rad/s² with a
  zero torque input, from 200 ms to 500 ms after that read T̂_d is within
  2 % of J · 50 rad/s² and every tick's e is within 0.01°.
- With b and T_c configured zero, a zero torque input, and T̂_d zero, a
  valid read at angle error e changes T̂_d by l_3 · e · T_s and, net of
  the cycle advance (fw~est_rotor_002~1), ω̂ by l_2 · e · T_s and θ̂ by
  l_1 · e · T_s, each within 1 %.
- With b and T_c configured zero, a zero torque input, and T̂_d zero, a
  tick with an invalid read leaves ω̂ and T̂_d unchanged and θ̂ to the
  cycle advance alone.
- At N_r = 5, a valid read after 6 consecutive invalid ones sets
  $\hat{x} = [\theta_m,\ 0,\ 0]^\mathsf{T}$ by the next cycle, at an angle
  error of 3 rad; a valid read after 5 applies K e.
- A configuration with ω_n at or below zero, ζ below 0.5, p_d at or below
  zero, or (2 · ζ · ω_n + p_d) · T_s above 1 is rejected at init.

Covers:
- sys~mc_001~1

Needs: impl, test

### Per-cycle rotor estimate
`fw~est_rotor_002~1`

Each bridge cycle callback (fw~io_bridge_007~1), the estimator shall
advance its state estimate as

$$\hat{x} \leftarrow F \hat{x} + G u, \qquad
F = \begin{bmatrix} 1 & T_{pwm} & 0 \\ 0 & 1 & 0 \\ 0 & 0 & 1 \end{bmatrix}, \qquad
G = \begin{bmatrix} T_{pwm}^2 / 2 \\ T_{pwm} \\ 0 \end{bmatrix}, \qquad
u = \frac{\tau - b \, \hat\omega - T_c \operatorname{sat}(\hat\omega / \omega_{db}) + \hat{T}_d}{J}, \quad \operatorname{sat}(x) = \max(-1, \min(1, x))$$

in scalar form θ̂ ← θ̂ + ω̂ · T_pwm + u · T_pwm² / 2, ω̂ ← ω̂ + u · T_pwm,
T̂_d unchanged, θ̂ wrapped to [0, 2π), and hold $\hat{x}$ as the rotor's
mechanical angle, velocity, and unmodelled-torque estimates for that
cycle, for T_pwm the configured PWM period, τ the torque input in Nm as
set by the motor-control application in the preceding callback
(fw~mc_016~1), zero from init, J the configured rotor inertia in kg·m², b
its configured viscous friction in Nm per rad/s, T_c its configured
Coulomb friction in Nm, ω_db the configured velocity in rad/s at which
the Coulomb term reaches ±T_c, and T̂_d the torque on the rotor beyond
those, positive in the sense of positive ω.

Acceptance:
- With no valid read, b and T_c configured zero, a zero torque input,
  T̂_d zero, and ω̂ = 21 rad/s, 20 callbacks advance θ̂ by
  20 · ω̂ · T_pwm within 1e-4 rad and leave ω̂ unchanged.
- With no valid read, b and T_c configured zero, T̂_d zero, and a torque
  input τ held from rest, 20 callbacks bring ω̂ to 20 · τ · T_pwm / J and
  θ̂ to 200 · τ · T_pwm² / J, each within 1 %.
- With b and T_c configured zero, a zero torque input, and T̂_d at a
  value D left by the corrections (fw~est_rotor_001~1), 20 callbacks with
  no valid read advance ω̂ by 20 · D · T_pwm / J within 1 % and leave
  T̂_d unchanged.
- With no valid read, ω_db = 2 rad/s, ω̂ = 21 rad/s, T̂_d zero, and a
  torque input of b · 21 rad/s + T_c, 20 callbacks leave ω̂ unchanged
  within 1e-4 rad/s.
- With no valid read, ω_db = 2 rad/s, ω̂ at +1 rad/s and at −1 rad/s,
  T̂_d zero, and a torque input of b · ω̂ + T_c · ω̂ / ω_db, 20 callbacks
  leave ω̂ unchanged within 1e-4 rad/s.
- At ω_n = 2π · 50 rad/s, ζ = 1, and p_d = 157 rad/s, with b and T_c
  configured zero and a zero torque input, from 100 ms after the first
  valid read of the rotor turning at a constant 21 rad/s under Gaussian
  encoder noise of 1.5 counts rms, every cycle's θ̂ over the following
  1 s is within 0.15° of the noise-free encoder angle.
- A configuration with an inertia at or below zero, a viscous friction
  below zero, a Coulomb friction below zero, or a Coulomb ramp velocity at
  or below zero is rejected at init.

Covers:
- sys~mc_001~1

Needs: impl, test
