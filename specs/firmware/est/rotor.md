---
status: draft
tags: [firmware, est, rotor]
---

# Rotor tracking estimator

The `app_rotorEstimation` module runs a fixed-gain tracking observer, in
state-space form, on each rotor's mechanical angle and velocity: one
estimator channel per rotor, configured with the encoder channel it reads
every millisecond, the loop's natural frequency and damping ratio, and the
PWM period of the bridge whose cycle callback carries the state forward.
The motor-control application commutates on the per-cycle estimate.

See also: [[encoder]] (the angle read, its direction, and its validity
are fw~est_encoder_004~1, fw~est_encoder_005~1, and fw~est_encoder_006~1),
[[motor-control-application]] (the per-cycle angle fw~mc_019~1 commutates
on the estimate; the alignment capture fw~mc_012~1 takes the offset from
it), [[bridge]] (the per-cycle callback, fw~io_bridge_007~1), [[status]]
(the velocity publication, fw~obs_status_001~1), [[motor-control]]
(sys~mc_001~1).

### Encoder tracking correction
`fw~est_rotor_001~1`

Each 1 ms tick, the estimator shall update its state estimate of
mechanical angle and velocity

$$\hat{x} = \begin{bmatrix} \hat\theta \\ \hat\omega \end{bmatrix}$$

zero from init, from the encoder channel's read as

| Read                                                   | Update |
|--------------------------------------------------------|--------|
| The first valid one since init                         | $\hat{x} = [\theta_m,\ 0]^\mathsf{T}$ |
| Every later valid one                                  | $e = \operatorname{wrap}(\theta_m - H\hat{x})$ to $(-\pi, \pi]$; $\hat{x} \leftarrow \hat{x} + K e$, $\hat\theta$ wrapped to $[0, 2\pi)$ |
| A failed one, or one the driver flags invalid (fw~est_encoder_006~1) | none |

with

$$H = \begin{bmatrix} 1 & 0 \end{bmatrix}, \qquad
K = \begin{bmatrix} K_p \, T_s \\ K_i \, T_s \end{bmatrix}, \qquad
K_p = 2 \zeta \omega_n, \quad K_i = \omega_n^2$$

in scalar form e = wrap(θ_m − θ̂); ω̂ ← ω̂ + K_i · e · T_s;
θ̂ ← wrap(θ̂ + K_p · e · T_s), taking effect by the next PWM cycle, for
θ_m the channel's reported angle in radians
(fw~est_encoder_004~1, fw~est_encoder_005~1), T_s the 1 ms tick, ω_n the
configured natural frequency in rad/s, and ζ the configured damping ratio.

Acceptance:
- At ω_n = 2π · 50 rad/s and ζ = 1, from the first valid read of a
  noise-free encoder angle advancing at a constant 21 rad/s, ω̂ is within
  1 % of the rate inside 50 ms and stays there over the following 1 s,
  through the encoder's 0/2π wraps.
- A valid read at angle error e changes ω̂ by K_i · e · T_s and, net of
  the cycle advance (fw~est_rotor_002~1), θ̂ by K_p · e · T_s, each within
  1 %.
- A tick with an invalid read leaves ω̂ unchanged and θ̂ to the cycle
  advance alone.
- A configuration with ω_n at or below zero, ζ below 0.5, or
  2 · ζ · ω_n · T_s above 1 is rejected at init.

Covers:
- sys~mc_001~1

Needs: impl, test

### Per-cycle rotor estimate
`fw~est_rotor_002~1`

Each bridge cycle callback (fw~io_bridge_007~1), the estimator shall
advance its state estimate as

$$\hat{x} \leftarrow F \hat{x}, \qquad
F = \begin{bmatrix} 1 & T_{pwm} \\ 0 & 1 \end{bmatrix}$$

in scalar form θ̂ ← θ̂ + ω̂ · T_pwm with ω̂ unchanged, θ̂ wrapped to
[0, 2π), and hold $\hat{x}$ as the rotor's mechanical angle and velocity
estimates for that cycle, for T_pwm the configured PWM period.

Acceptance:
- With no valid read and ω̂ = 21 rad/s, 20 callbacks advance θ̂ by
  20 · ω̂ · T_pwm within 1e-4 rad.
- At ω_n = 2π · 50 rad/s and ζ = 1, after 100 ms of the rotor turning at a
  constant 21 rad/s under Gaussian encoder noise of 1.5 counts rms, every
  cycle's θ̂ over the following 1 s is within 0.15° of the noise-free
  encoder angle.

Covers:
- sys~mc_001~1

Needs: impl, test
