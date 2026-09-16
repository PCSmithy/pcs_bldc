---
status: draft
tags: [firmware, mc, app, driver]
---

# Frame transforms and space-vector modulation

Pure functions shared by the commutation methods: the transforms between
phase, stationary (α, β), and rotor (d, q) quantities, and the space-vector
modulator that turns a stationary-frame voltage command into three
per-phase duties.

See also: [[vf-sinusoidal]] (fw~mc_010~1 commands its voltage through the
modulator), [[motor-control-application]] (fw~mc_016~1 observes the
rotor-frame currents), [[motor-control]] (sys~mc_001~1).

### Frame transforms
`fw~mc_013~1`

The transforms shall map between the phase, stationary, and rotor frames
with amplitude-invariant scaling, θ the frame's electrical angle:

| Transform    | Input → output     | Definition                                    |
|--------------|--------------------|-----------------------------------------------|
| Clarke       | (a, b, c) → (α, β) | α = (2a − b − c) / 3, β = (b − c) / √3        |
| Park         | (α, β, θ) → (d, q) | d = α cos θ + β sin θ, q = −α sin θ + β cos θ |
| Inverse Park | (d, q, θ) → (α, β) | α = d cos θ − q sin θ, β = d sin θ + q cos θ  |

Acceptance:
- Each transform matches a reference implementation within 1e-5 of the
  input magnitude over a sweep of angles and amplitudes.
- Park followed by inverse Park at the same angle returns the input.
- A balanced three-phase set of amplitude A maps to |(α, β)| = A.

Covers:
- sys~mc_001~1

Needs: impl, test

### Space-vector modulation
`fw~mc_014~1`

The modulator shall produce per-phase duties d_u, d_v, d_w from a
stationary-frame voltage command (v_α, v_β) and the bus voltage V_bus as

v_u = v_α, v_v = (−v_α + √3 v_β) / 2, v_w = (−v_α − √3 v_β) / 2

d_x = 0.5 + (v_x − (max(v_u, v_v, v_w) + min(v_u, v_v, v_w)) / 2) / V_bus

where a command whose magnitude exceeds V_bus / √3 is first scaled to
magnitude V_bus / √3 at its own angle, and a bus voltage of zero yielding
duties of 0.5.

Rationale:
- Sector-based SVM and min/max zero-sequence injection yield the same duty
  triple under symmetric zero-vector placement; pinning the triple keeps
  either formulation compliant.

Acceptance:
- Every duty lies in [0, 1] for any command.
- d_x − d_y = (v_x − v_y) / V_bus for every phase pair, for commands within
  the linear limit.
- Over one electrical cycle at magnitude V_bus / √3, the duties reach both
  0 and 1.
- Duties match a reference implementation within 1e-5 over a sweep of
  angles and magnitudes, including magnitudes beyond the linear limit.

Covers:
- sys~mc_001~1

Needs: impl, test
