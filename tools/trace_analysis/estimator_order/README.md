# Estimator order characterisation

The test plan and analysis for the rotor estimator's order decision: whether
the second-order tracking observer with the known-input term (design B) is
the estimator to keep, whether the plain second order (design A) was better,
or whether a third order with a disturbance state (design C, now the
`mc_foc` head) is worth having. Same sequences in SIL and on the bench, one
notebook over both.

## Designs

| Design | What | Where |
|--------|------|-------|
| A | Second-order tracking observer, constant-velocity prediction | `90d5914` (`fw~est_rotor_001/002` before the input term); `build_design.sh A` rewrites the head's estimator to it (u = 0, second-order gains, no torque state) |
| B | A plus the known-input term G·u, u = (K_t·I_q − b·ω̂)/J | `3d90e0e` to `f949d1d`; `build_design.sh B` |
| Bc | B with the Coulomb term in u: C with its torque state held at zero | `build_design.sh Bc` |
| C | B plus a disturbance state T̂_d absorbing the unmodelled load, the Coulomb term in u | `mc_foc` head; `fw~est_rotor_001/002` as written |

A's weakness is lag under acceleration: angle a/K_i, velocity 2ζa/ω_n, a
time lag of 2ζ/ω_n = 6.4 ms at the board's ω_n = 2π·50, ζ = 1. B removes it
when K_t/J and b are right and the load is the model's; what it cannot model
(a wrong J, Coulomb friction, a hand on the shaft) the correction loop has to
cancel as a residual, whose steady part C would absorb.

## Scenarios

The torque runs start from rest with the drive enabled and aligned, under
the torque method; the velocity run under the velocity method. Each is one
`Command`: the points below, then the stop at 1000 ms that every sequence
run carries. The voltage ceiling (the back-EMF at 200 rpm is the 20 V bus's
limit, about 20 rad/s) pins the plant and collapses the delivered torque, so
S2 and S3 stay under it; S1's 0.08 Nm step brushes it at the low-inertia
variant, which is accepted for the acceleration it buys: 250 rad/s², where A
lags 0.15° in angle and 1.6 rad/s in velocity.

| Scenario | Mode | Points (t ms, target, rate limit) | Exercises |
|----------|------|-----------------------------------|-----------|
| `S1_step` | torque | (100, 0.08 Nm, 0), (160, 0.01, 0), (700, 0, 0) | A step of acceleration, a slow decay, a coast to rest under Coulomb friction |
| `S2_ramp` | torque | (100, 0.04 Nm, 0.25 Nm/s), (260, 0.01, 0.25), (800, 0, 0.25) | A torque triangle: ramped acceleration, the input term's tracking |
| `S3_reversal` | torque | (100, 0.03 Nm, 0), (300, −0.03, 0), (600, 0.03, 0), (900, 0, 0) | Reversals through zero speed, where the friction model is wrong; decorrelates acceleration from velocity for the residual fit |
| `S4_velstep` | velocity | (100, 10 rad/s, 0), (700, 0, 0) | The estimator's consumer: the velocity loop's own I_q feeds B's input term |
| `S5_crawl` | velocity | (100, 0.5 rad/s, 0) | A crawl held through the stop: the friction model's ramp band and stiction, where the slow-speed roughness lives |

SIL runs every scenario for every design on four plant variants: the inertia
at 0.5×, 1× and 2× the estimator's configured J (read out of the firmware
under test, so the 1× variant tracks the config), and the Coulomb friction
at 4× the plant model's 4.4 mNm (17.6 mNm) as the unmodelled load C absorbs.
Every design runs on the same variants, so each comparison is on the same
plant: with f the plant's J over the configured one, B's lag is (1 − f) of
A's in the linear regime. The SIL plant's own friction (Coulomb 4.4 mNm,
viscous 7.2e-4) differs from the estimator's configured values, so in SIL
T̂_d carries the difference at the 1× variant by construction: +5.6
mNm·sign(ω) at the 10 mNm / 7.2e-4 configuration the 2026-11-07 captures
ran with, +10.6 mNm·sign(ω) − 6.2e-4·ω at the 15 mNm / 1e-4 one.

## Signals

The DWARF paths below are the column names in every capture, SIL or bench.
K_t·I_q, the estimator's torque input, is derived from I_q in the notebook.

| Path | Signal | Watch period (the panel's label) |
|------|--------|--------------|
| `app_rotorEstimation_data.channels[0].x_hat.e[0][0]` | θ̂, rad | 20 kHz |
| `app_rotorEstimation_data.channels[0].x_hat.e[1][0]` | ω̂, rad/s | 20 kHz |
| `app_rotorEstimation_data.channels[0].x_hat.e[2][0]` | T̂_d, Nm (design C) | 20 kHz |
| `app_motorControl_data.channels[0].iQ_a` | I_q, A | 20 kHz |
| `app_rotorEstimation_data.channels[0].angleMeasured_rad` | θ_m, the last valid encoder angle | 1 ms |
| `app_rotorEstimation_data.channels[0].angleError_rad` | e = θ_m − θ̂ at the last correction | 1 ms |
| `app_rotorEstimation_data.channels[0].u` | the advance's modelled torque, Nm (an acceleration in rad/s² in the A and B bench captures) | 1 ms |
| `app_motorControl_data.channels[0].torqueSetpoint_Nm` | the torque target in effect | 1 ms |
| `app_motorControl_data.channels[0].velocitySetpoint_radPerSec` | the velocity target in effect | 1 ms |

SIL captures add `plant.angle_rad` and `plant.velocity_radps`, the truth the
bench does not have. Nothing the bench metrics need runs faster than the
1 kHz correction; the fourth one-cycle slot (`fw~conn_trace_001`) is free.

## Procedure, SIL

```bash
tools/trace_analysis/estimator_order/run_scenarios.sh
```

Rebuilds every DLL (C from the branch's release build; A, B and Bc through
`build_design.sh <design>`, which rewrites the estimator source in place,
builds into `build/native-fw-est<design>` and restores the file on exit —
nothing else may build or commit meanwhile), then runs
`sw/sil/pcs_bldc_sil/tests/estimator_order.rs` for each; design letters as
arguments run a subset. The captures land in `sil/` as
`<scenario>_<design>_J<factor>[_Tc<factor>].csv`, one row per PWM cycle,
with `provenance_<design>.txt` naming the commit; regenerable, not
committed.

## Procedure, bench

1. Flash the design under test: C is the branch. For a comparison design,
   apply `build_design.sh`'s rewrite by hand (or run it and copy the
   rewritten file out before it restores), `tools/build_arm.sh`, flash,
   then restore the file. The app's identity gate wants the ELF of the
   image running, so point it at the right build.
2. Watch the nine signals above at the periods in the table.
3. Command widget: FOC torque, 0 Nm, Send. The first enable after power-up
   aligns; wait for the ring to settle.
4. Sequence widget: enter each scenario's points under its mode (the
   tooltip takes typed times and values), duration 1000 ms, and save it by
   name, so the same sequence runs for both designs.
5. Run. Pause after the stop, select the range from a little before the run
   mark to 300 ms past the stop (the coast-down carries b and the Coulomb
   friction), Export, and save as `bench/<scenario>_<design>_r<n>.csv`.
6. Three repeats per scenario and design, for the run-to-run spread (cogging
   position, temperature). The repeats may share one capture: Run again once
   the rotor has rested at least half a second after the previous stop, then
   export the whole span; the notebook splits a capture at every such rest
   and numbers the runs `r<n>.1`, `r<n>.2`, … Then one `S1_step` per design with a deliberate
   drag on the shaft, as `bench/S1_drag_<design>_r<n>.csv`: the load case
   for C. For C itself, add a run with a known load, a weight hung from a
   string on the shaft, as `bench/S1_weight_C_r<n>.csv`, so T̂_d reads
   against a torque that is known.
7. Commit the bench captures.

If 0.08 Nm does not break the real rotor away, or overspeeds it, scale the
torque scenarios together and note the factor in the findings; the
notebook's under-torque mask is relative to each run's peak target.

## Metrics

Computed in `estimator_order.ipynb` over the active window, the first
nonzero target to its last, with time rebased so every run's first target
sits at 0.1 s. The per-cycle peaks sit on the encoder noise (±0.1°, RMS
0.04°), so the designs separate in the **signed means**: each error sample
weighted by the sign of the target in effect, averaged over the cycles under
real torque (the delivered K_t·I_q, 10 ms mean, past half the run's peak
target). A's lag is negative there; a wrong J errs either way.

- SIL only: the per-cycle angle error θ̂ − θ_plant, signed mean, peak, RMS;
  the velocity error, signed mean and peak; the velocity lag, the least
  mean square shift of ω̂ over the plant, searched both ways.
- SIL and bench: the correction residual e at 1 kHz, signed mean, peak, RMS,
  and its mean over the last 200 ms of the window; ω̂ against the encoder's
  own Δθ/Δt at the interval midpoints, signed mean error and lag.
- The **residual decomposition**, both sides: e ≈ c_a·a + c_ω·ω̂ +
  c_c·sign(ω̂) + c_0 fitted at the correction instants, with a from the
  encoder velocity. It gives the input-term fraction f = 1 − K_i·c_a (0 for
  A, J_plant/J_configured for B with K_t right), the encoder's propagation
  delay t_prop = −c_ω, and the load terms c_c, c_0 in degrees.

The AS5048 reports its angle about 100 µs late (t_prop, datasheet p. 9):
−0.057° of residual at 10 rad/s, the sign of a J set too low. The SIL
encoder model carries no t_prop, so the fit is what keeps the bench J
reading honest; modelling t_prop in `sw/sil/pcs_bldc_sil/src/as5048.rs` is
a SIL backlog item.

## Decision

Proposed criteria, to be confirmed against the data:

- Keep B, and set the configured J from the bench's fitted f, if f is
  consistent across the bench repeats (spread under 20 %) and the velocity
  run shows B's signed velocity error under A's.
- Build C if the bench load terms c_c + |c_0|, or the drag run's residual,
  cost more than 0.1 rad/s of velocity bias (2ζ·K_i·e/ω_n, 1 % of the
  10 rad/s step the velocity acceptance uses), which is a residual load
  term past 0.01°. SIL's 4.4 mNm Coulomb friction alone sits at 0.0085°,
  so the bench decides.
- Revert to A only if B at the fitted J is worse than A on the same plant,
  which would mean the input term is wrong, not the order.

The decision settles the four SIL tests that fail by design of B
(`rotor_currents::current_in_quadrature_reads_as_iq`,
`rotor_estimate::the_velocity_converges_and_holds_through_wraps`,
`::the_estimate_tracks_a_21_rad_s_encoder_every_cycle`,
`::invalid_reads_coast_the_estimate_on_its_velocity`): their expectations
follow the chosen design.

## Findings

### SIL, 2026-11-03 (captures from `de51b05` with this folder uncommitted)

Signed means under torque, from the notebook's first table; S2 and S3 now
stay under the voltage ceiling.

| Run | angle signed ° | vel signed rad/s | vel lag ms | e signed ° |
|-----|---------------:|-----------------:|-----------:|-----------:|
| S1 A J×1 | −0.077 | −1.27 | 6.3 | +0.114 |
| S1 B J×0.5 | −0.065 | −0.97 | 3.0 | +0.087 |
| S1 B J×1 | +0.016 | +0.11 | −0.2 | −0.009 |
| S1 B J×2 | +0.061 | +0.76 | −6.2 | −0.071 |
| S1 B J×1 Tc×4 | +0.022 | +0.37 | 0.2 | −0.034 |
| S3 A J×1 | −0.029 | −0.48 | 6.4 | +0.044 |
| S3 B J×1 | +0.002 | +0.05 | −0.2 | −0.004 |
| S4 A J×1 | −0.077 | −1.16 | 6.0 | +0.100 |
| S4 B J×1 | +0.019 | +0.13 | −0.1 | −0.012 |

- A trails the plant by 6.3 ms in velocity on every plant, the 2ζ/ω_n =
  6.4 ms of its constant-velocity model, and its signed angle lag scales
  with the plant's acceleration (−0.115° at J×0.5, −0.030° at J×2 in S1).
  B at the right J removes the lag and leaves the angle bias inside the
  noise, in every scenario, the velocity step included.
- A wrong J scales B's input term: at J×0.5 B keeps half of A's lag
  (3 ms), at J×2 it leads by the same 6 ms it would have trailed. The
  signed residual carries the bias with the opposite sign to the angle
  error (A: +0.114° against −0.077°; the correction removes 63 % of the
  error each 1 ms, so the per-cycle mean is about 0.68 of the residual).
- The residual decomposition recovers the plant's inertia from the
  residual alone, on the torque runs: f = 0.53, 1.07, 2.38 for B and
  0.56, 1.04, 2.30 for A at J×0.5, 1, 2 in S1; 0.52, 1.02, 1.89 (B) in S3.
  S2's gentler acceleration compresses the high end (1.7); the velocity
  step is not a usable fit (its ω̂ regressor is the step itself). The
  encoder-delay term reads within ±40 µs of zero on the torque runs, the
  readout's resolution for the bench's 100 µs; S3 is the sharpest (±14 µs).
- The unmodelled load: 4× Coulomb (17.6 mNm) costs B a +0.37 rad/s signed
  velocity error and a −0.03° residual, the load term c_c = −0.03° in the
  fit, with f unchanged (1.02 in S3). On A the same load folds into the
  inertia reading (f 1.19 to 1.37). The board's own 4.4 mNm leaves
  nothing visible.
- Against the plan's criteria: B at J×1 meets them; the C threshold as
  written (0.1 rad/s of velocity bias, 0.01° of load term) is crossed by
  the 4× Coulomb variant and not by the board's nominal friction, so the
  bench's real load decides. The decision needs the bench.

### Bench, design A, 2026-11-04

Torques: S1 at half the plan (0.04 / 0.005 Nm), S2 and S3 at the plan's
(0.04 peak; ±0.03), the velocity step as planned. The rotor is lighter
than the configured J, so S2's faster runs and S3's legs reach the 20 rad/s
ceiling; the under-torque mask drops those stretches. 6, 16, 14 and 13
runs, one capture each.

- A's signature is the SIL's: the estimate trails the encoder's own
  velocity by 6 ms in every torque run, and the signed residual under
  torque is 0.10° (S1), 0.06° (S2), 0.06° (S3), repeatable to about
  ±0.01° across the runs.
- The fitted inertia ratio is 0.55 (S1), 0.65 (S2) and 0.72 (S3,
  one stuck run excluded): the rotor's inertia is about two thirds of the
  configured 3.0e-4 kg·m², around 2.0e-4. S1's breakaway from stick pulls
  its reading low.
- The encoder-delay term reads within ±20 µs of zero on the reversals,
  where it is best separated: the 100 µs the datasheet gives is not in
  the residual, so the firmware's sampling already absorbs it and the
  inertia reading needs no latency correction.
- The load terms are small: the sign-of-velocity term is under 0.02°
  on the reversals. The coast-down after S1 says the Coulomb friction is
  near 10 mNm, above the SIL model's 4.4, and varies with position.
- The velocity step overshoots to 16 rad/s on a 10 rad/s target and rings
  at about 50 Hz for 100 ms, with the residual swinging ±1°. The loop's
  gains were derived for the configured J, and A's 6 ms velocity lag sits
  inside the loop; this is the run B should change most.

### Bench, design B, 2026-11-04

The same sequences as A (15, 15, 14 and 13 runs). A first S1 capture ran
at the plan's 0.08 Nm, not A's halved 0.04, and is kept as `S1_full_B_r0`
(0.129° signed residual at twice the acceleration, half what A showed at
that torque in the deleted full-torque run); the halved S1 is the rerun.

| Like for like | A | B |
|---|---:|---:|
| S1 signed residual under torque | 0.101 ± 0.006° | 0.045 ± 0.006° |
| S1 ω̂ lag against the encoder | 6 ms | 4 ms |
| S2 signed residual under torque | 0.061 ± 0.004° | 0.012 ± 0.011° |
| S3 signed residual under torque | 0.061 ± 0.001° | 0.034 ± 0.003° |
| S2 / S3 ω̂ lag against the encoder | 6 ms | 4 ms / 2 ms |
| S4 velocity-loop overshoot | 71 ± 4 % | 31 ± 16 % |
| S4 residual peak through the step | 1.00° | 0.56° |

- B beats A on every like-for-like run: the residual under torque halves
  or better, the lag drops from 6 ms to 2 to 4 ms, and the velocity loop's
  overshoot on the 10 rad/s step falls from 71 % to 31 % with the ringing
  shorter, the consumer-level gain the input term was built for.
- What B keeps of A's lag is the configured inertia: the fit reads the
  rotor at 0.44 to 0.58 of the configured J on B's runs (0.55 to 0.73 on
  A's), so the input term runs at about half strength and B keeps about
  half of A's lag, which is what the residuals show. Setting J to about
  1.6e-4 kg·m² should take the rest out; S3 is the run to confirm it on.
- The load term is now visible: on the reversals B's sign-of-velocity
  term is −0.025 ± 0.005°, the rotor's roughly 10 mNm Coulomb friction
  that A's constant-velocity model never fed and B's input term leaves
  uncancelled. At the plan's conversion that is about 0.3 rad/s of
  velocity bias at low speed, past the 0.1 rad/s line drawn for building
  C, but measured with the inertia still wrong; the corrected-J rerun and
  the drag run decide it.
- The encoder-delay term stays within 40 µs of zero on every torque run,
  so no latency correction enters the inertia reading.

### Bench, design B at the fitted inertia, 2026-11-04

The configured J set to 1.6e-4 kg·m² (`Jcfg1.6` captures): S3, S4, and
S1 under finger drag, 14, 13 and 15 runs.

| | B, J 3.0e-4 | B, J 1.6e-4 |
|---|---:|---:|
| S3 signed residual under torque | 0.034 ± 0.003° | −0.001 ± 0.002° |
| S3 ω̂ lag against the encoder | 2 ms | 0 ms |
| S3 fitted rotor inertia | 1.62e-4 | 1.44e-4 |
| S3 sign-of-velocity load term | −0.026° | −0.042 ± 0.005° |
| S4 velocity-loop overshoot | 31 ± 16 % | 10 ± 5 % |
| S4 residual peak through the step | 0.56° | 0.23° |

- With the inertia right the input term removes A's lag entirely: the
  reversals show no lag and a residual under torque at the noise, and the
  velocity loop's overshoot on the 10 rad/s step falls to 10 % from A's
  71 %, the residual through the step a quarter of A's.
- The fit now reads the rotor at 1.44e-4 on the reversals, so 1.6e-4 is
  still about 10 % high; the remaining 0.5 ms of the velocity step's lag
  spread is consistent with that.
- What remains is the load the model does not carry: the sign-of-velocity
  term is −0.042° on the reversals, which at the plan's conversion is
  0.45 rad/s of velocity-estimate bias whenever the rotor turns, flipping
  sign with direction. That matches a Coulomb friction of about 10 mNm at
  the fitted inertia, the same figure the S1 coast-downs gave.
- The finger-drag runs are too uneven to read a load from: the inertia
  reading scatters 0.7 to 1.3 and the load terms with it. They show the
  machinery copes (no stall, residual peaks unchanged) and no more.

### SIL, design C, 2026-11-07 (all four designs rebuilt from the head, J_cfg = 1.6e-4)

Every design on every variant, A, B and Bc rewritten from the head by
`build_design.sh`, so the comparison is at one configuration. Numbers at the
1× inertia unless said; the table and the panels are the notebook's.

- The steady residual is gone: C's `e tail` is 0.000–0.001° on every run
  where B holds −0.005 to −0.03° and Bc +0.013 to +0.028°. The angle rms
  rises a little, 0.043° against B's 0.036° on S1, the third integrator's
  noise; the peaks are the same 0.14–0.16°.
- The inertia mismatch no longer costs velocity: at 0.5× the velocity lag
  against the plant is A 6.2, B 3.0, Bc 3.6, C 1.4 ms and the signed
  velocity error under torque on S1 is A −3.4, B −0.97, Bc −1.95, C −0.58
  rad/s; at 2× the lag is B −6.2, Bc −4.4, C −0.25 ms and the signed error
  B +0.76, Bc +0.97, C +0.12 rad/s. At 1× all three input-term designs sit
  near zero.
- T̂_d reads the load it was built to read: at 1× it settles at 5.3, 5.3,
  5.6 and 4.9 mNm on S1–S4 against the +5.6 mNm the Coulomb mismatch puts
  there by construction; on the 4× Coulomb variant at −10.5, −10.2, −6.4
  and −7.6 mNm against −7.6 (S1 and S2 run one way, so their sign term and
  constant are not separable and the coast-down leaks in; S3's reversals
  give −5.7 mNm on the sign term, −7.6 expected).
- The inertia read out of T̂_d (the notebook's lag-compensated fit) lands
  at 0.50–0.52, 1.01–1.03 and 2.03–2.23 of the configured J on the torque
  scenarios S1–S3 for plants at 0.5×, 1× and 2×; the velocity scenario's
  closed loop spoils it (0.65, 1.15, 2.39). The bench's inertia reading now
  comes from this fit rather than the residual's.
- The velocity step's overshoot at 1× is A 61 %, B 7.8 %, Bc 8.5 %, C
  5.1 %; at 2× C is worse (14.7 % against B's 8.9 %) and at 0.5× better
  (8.9 % against 18.9 %). The loop's gains were derived for the configured
  J, and C's torque state fights the loop's integrator on the heavy plant.
- Bc hardly differs from B here because the SIL plant's Coulomb friction is
  4.4 mNm and the configured 10 mNm over-compensates; on the bench, where
  the 10 mNm was measured, Bc is the fair B-plus-Coulomb point.
- With b/J at the fitted inertia (4.5 s⁻¹), B fails two of the SIL
  estimator checks in the hand-driven-encoder world, where the model's
  deceleration has no plant to match: the velocity holds 3 % low and the
  per-cycle angle peaks at 0.19°; C passes both (0.006 %, 0.096°), the
  torque state absorbing the model's friction. The current-observation
  check that injects I_q on a rotor that does not move needs 50 ms rather
  than 10 for T̂_d to absorb the torque input the injection implies; under
  B that frame error was a permanent 36°.

### Bench, design C, 2026-11-07

`bench/S{1..4}_*_C_r0.csv`, twelve to fourteen runs of each scenario in one
capture, against the design-B runs at the fitted inertia (`B Jcfg1.6`) from
2026-11-04. No weight run: the rig for a hung weight is not to hand, and the
steady-speed segments stand in for it (below).

- The steady residual and the velocity bias are gone on the bench as in
  SIL: `e tail` 0.000 ± 0.001° on every scenario (B −0.035° on S4 and on
  the drag run), the signed residual under torque −0.009° on S4 against
  B's −0.049°, and ω̂ against the encoder's own Δθ/Δt within 0.04 to 0.18
  rad/s where B held 0.4 to 0.6.
- The velocity step overshoots 7.8 ± 0.3 % against B's 10.4 ± 5.4 %, and
  settles without B's and A's ringing (the S4 panel).
- The costs: at the reversals T̂_d has to swing about 25 mNm as the
  Coulomb term in u flips sign, and the residual peaks at 0.26° against
  B's 0.23° while it does (about 20 ms, the 157 rad/s pole); the residual
  rms is otherwise unchanged (0.045° against 0.043° at rest). At standstill
  the velocity estimate's rms is 0.11 rad/s against B's 0.083: the
  ±10 mNm Coulomb term toggles with the sign of ω̂ each cycle, a 3e-3 rad/s
  kick per cycle that the 1 ms corrections then take out. Position control
  would see that as velocity noise at rest; the Coulomb term is a ramp
  through zero for it (fw~est_rotor_002, ω_db).
- T̂_d reads the friction model's error, and with it the rotor's actual
  friction (the notebook's new *Friction curve* cell): the drag is 14 to
  17 mNm from 1 to 17 rad/s forward, flat within the scatter, where the
  configured model (b = 7.2e-4, T_c = 10 mNm) climbs from 11 to 23 mNm. At
  the one steady reverse speed, −20 rad/s, the drag is 10 to 13 mNm against
  the model's 24. The delivered torque at steady speed agrees with the drag
  so read within 1 to 2 mNm (16.7 against 16.6 mNm on the 10 rad/s hold),
  which is the calibration the weight run was for. The friction is
  Coulomb-like at about 15 mNm with little viscous part; the configured b
  is what T̂_d spends most of its range cancelling (+10 mNm at 20 rad/s).
- At standstill under a torque below breakaway (S1's 10 mNm hold after
  the step, where the rotor stops by 0.3 to 0.5 s) T̂_d sits at −10.3 ±
  0.5 mNm: with sign(0) = 0 the model offers no friction at rest, so the
  state carries the whole applied torque, i.e. it reads the stiction. The
  breakaway is above 10 mNm, consistent with the 15 mNm Coulomb figure.
- The inertia read out of T̂_d is less consistent than B's residual gave,
  because the friction model's error is a velocity term the fit has no
  regressor for: S3 0.89 ± 0.06 (B's residual fit 0.90 ± 0.02), S1 0.75 ±
  0.10, S2 1.14 ± 0.16, S4 1.21 ± 0.09. With an ω regressor added, the
  S3 fit returns 0.79 and a viscous mismatch of +0.79 mNm per rad/s, which
  is the configured b itself: the data want b near zero. The inertia
  reading from S3, the scenario built to separate acceleration from
  velocity, stays at 1.4 to 1.5e-4 kg·m², a tenth under the configured
  1.6e-4.

### Bench, slow velocity holds before and after the T̂_d feed-forward, 2026-11-08

`bench/slow_foc_vel_cmd_cogging.csv` (design C, 2026-11-07 firmware) and
`bench/slow_foc_vel_cmd_cogging_td_ff.csv` (the same with fw~mc_025's T̂_d
feed-forward at k_ff = 1, fw~est_rotor_002's Coulomb ramp at ω_db = 1 rad/s,
and the friction re-fit T_c = 15 mNm, b = 1e-4): two repeats of 0.1 → 0.5 →
0.1 → 0 rad/s under the velocity method, 30 s each. The capture that
motivated the two changes, then the check.

- **Before.** At 0.5 rad/s the velocity ripples ±0.2 rad/s at 2.3 Hz, a
  clean 2 cycles per electrical revolution (28 per turn): a 9 mNm torque
  ripple that T̂_d tracks in antiphase and the loop fights with 9 mNm of
  I_q. The PI's rejection at 2.3 Hz, |K_vp + K_vi/jω| ≈ 0.054 Nm per
  rad/s, predicts 0.17 rad/s of ripple for it; 0.2 is seen. A 2ω_e
  torque ripple is the signature of a phase-current sense gain or offset
  imbalance (the bridge config has one shared scale for all three
  phases), or of a magnet or back-EMF asymmetry. At 0.1 rad/s the rotor
  sticks 17 % of the time against 19 mNm of friction and breaks away to
  0.3 to 0.6 rad/s a few times a second while the loop's integrator winds
  up; T̂_d reads the stiction at −20 to −30 mNm; ω̂ is mostly noise
  (0.15 rad/s rms on 0.1 of motion) and its sign flips 1400 times a
  second, toggling the ±10 mNm sgn term into u.
- **After.** The 2-per-cycle velocity ripple at 0.5 rad/s is 0.20 →
  0.06 rad/s while T̂_d still carries the full 8 to 10 mNm of it: the
  disturbance observer cancels what it was built to cancel. Velocity rms
  deviation 0.20 → 0.11 rad/s at 0.5 and 0.10 → 0.05 at 0.1. Stick-slip
  is mostly gone: stuck 16.6 → 2.0 %, excursions −0.33..+0.59 →
  −0.03..+0.38 rad/s and 6 ms wide, T̂_d no longer winding during a
  stick. Inside the ramp band the Coulomb term is only 1.5 mNm, so T̂_d
  sits at −15.6 mNm carrying the friction and k_ff feeds it straight
  into the demand, which is why the integrator no longer has to wind up
  before breakaway. ω̂'s sign flips fall to 480 per second.
- **What remains.** The dominant ripple at 0.5 rad/s is a 13.1 to 13.4 Hz
  line of 0.09 to 0.10 rad/s amplitude, with weaker lines at 6.9 and
  27.7 Hz. Read on 2026-11-08 as a loop mode of the feed-forward, because
  its phase against the angle wandered between repeats; the 2026-11-09
  captures below correct that. It moves with speed (2.75 Hz at 0.1
  rad/s), so it is the 6, 12 and 24 per electrical cycle series,
  inverter dead-time distortion at the phase-current zero crossings. A
  3.5 s hold resolves the order only to about 4 cycles per turn, which
  is what let the phase wander.
- **Costs.** Torque dither K_t·I_q rms 17 → 18.7 mNm; standstill ω̂ noise
  0.107 → 0.133 rad/s rms. The feed-forward closes a loop from T̂_d noise
  through the current loop back into the estimator. u's standstill noise
  is unchanged at 16 mNm and is current-measurement noise, not the
  Coulomb term, which the ramp fixed.

### Bench, the first cogging compensation, 2026-11-09

`bench/slow_foc_vel_cmd_cogging_td_ff_2.csv`: the 0.5 and 0.1 rad/s holds
again with fw~mc_028 in at k_cog = 1 and the model fitted from the open-loop
crawl (28 per turn 11 mNm at 1.08 rad, 56 per turn 4.5 mNm at −2.13 rad),
then a 0.05 rad/s crawl for 47 s. T̂_d now reads the residual, so its
harmonic fit against the raw encoder angle, with the observer's lag at the
hold speed advanced out, gives the true term as model plus residual.

| Order | Before, T̂_d | After, T̂_d residual | True term | Before, velocity | After, velocity |
|---|---|---|---|---|---|
| 28/rev | 8.3 mNm at 54° | 2.2 mNm at −115° | 9 mNm at 60 to 70° | 0.06 rad/s | 0.01 rad/s |
| 56/rev | 0.5 to 0.9 mNm | 3.7 mNm at +56° | 0.9 mNm at −100° | 0.01 rad/s | 0.05 rad/s |

- The 28-per-turn term works: its velocity ripple falls six-fold, the
  configured phase is right within a few degrees, and the amplitude is a
  quarter high (the residual sits in antiphase to the model).
- The 56-per-turn term is wrong by four-fold. Under the field-oriented
  methods the motor's own 56-per-turn content is under 1 mNm; the 4.5 mNm
  came from the open-loop sinusoidal crawl, where the drive's 2- and
  4-per-electrical-cycle torque ripple lands on the same mechanical orders
  as the cogging and cannot be told from it. The compensator injected
  3.7 mNm at 56 per turn that the motor never had, so the overall velocity
  ripple did not move (0.111 to 0.112 rad/s rms at 0.5 rad/s): the energy
  moved from 2.2 to 4.5 Hz, which is the roughness that remained.
- The estimator's angle tracks the raw encoder within 0.04°, so the
  reference angle is not in question.

The amplitudes belong to the field-oriented hold's T̂_d, not the open-loop
crawl. Config after this capture: 28 per turn 9 mNm at 1.1 rad, 56 per turn
0.9 mNm at −1.75 rad.

`bench/slow_foc_vel_cmd_cogging_td_ff_3.csv`, the same holds on that
config:

| Order | T̂_d residual at 0.5 rad/s | True term, model plus residual | Velocity ripple |
|---|---|---|---|
| 28/rev | 1.6 mNm | 9.2 mNm at 73° | 0.01 rad/s |
| 56/rev | 1.1 mNm | 0.8 mNm | 0.01 rad/s |

- Both terms are cancelled to the noise and the fit has converged on the
  configured values. The 0.1 rad/s hold is the smoothest captured: 0.051
  → 0.044 rad/s rms, stuck 2.0 → 1.1 %, no reversals.
- The 0.5 rad/s hold stays at 0.111 rad/s rms, because what is left
  there is not cogging: a 13.7 Hz line of 0.085 rad/s with 6.9 and
  27.7 Hz beside it, at 2.75 Hz on the 0.1 rad/s hold in every capture.
  It scales with speed, so it is position-referenced, and the series is
  6, 12 and 24 per electrical cycle: inverter dead-time distortion. TIM1
  runs 3600 counts centre-aligned at 144 MHz with 43 dead-time ticks,
  about 300 ns before the gate driver's own minimum, 0.6 % of the 50 µs
  period or about 0.12 V per phase at a 20 V bus; the hold's 25 mA of I_q
  needs about 0.65 V, so the error is a fifth of the drive voltage, and
  the current loop attenuates an 86 rad/s disturbance about tenfold.
- Removing that line entirely would leave about 0.09 rad/s rms, the
  encoder noise through the loop (the SIL crawl floor), and sign-of-current
  compensation at 25 mA sits inside the current-sense noise. Not worth
  it here; low-speed smoothness is called good at this point. The lever
  on what remains is the encoder noise itself.

### Decision

Against the plan's criteria, B stays: at the fitted J it beats A on every
run, the inertia reading is consistent across the bench repeats, and
nothing argues for reverting. The load term is past the 0.1 rad/s line,
so the plan says build C — with one caveat the data adds. The load the
bench shows is Coulomb friction, a known shape with one parameter, and B's
input model can carry it directly, u = (K_t·I_q − b·ω̂ − T_c·sign(ω̂))/J,
at the cost of a configured T_c measured from the coast-down and no new
state. C's disturbance state buys what that does not: a load the model
cannot name, a hand on the shaft or a mechanism, which position control on
a real load will meet. The order of work this suggests is the Coulomb term
in B first, re-read the load term on S3, and open C when a load the
sign-of-velocity term cannot explain shows up.

Overridden 2026-11-04: position control on an unknown load is the next
sprint item, so C goes in now, while the plan and the bench rig are in
hand, rather than after a Coulomb-only B. The Coulomb term rides along as
C's known-input model, so B-plus-Coulomb remains a comparison point: it is
C with T̂_d held at zero. The specs are `fw~est_rotor_001~1` and
`fw~est_rotor_002~1` in `specs/firmware/est/rotor.md`; C's gains with the
friction correction, l₁ = 2ζω_n + p_d − b/J, l₂ = ω_n² + 2ζω_n p_d −
l₁ b/J, l₃ = J ω_n² p_d, place the error poles at the roots of
(s² + 2ζω_n s + ω_n²)(s + p_d).

SIL, 2026-11-07: C does what it was built for (the steady residual and
the inertia-mismatch velocity error gone, T̂_d reading the known load
within 10 %, the inertia within 5 % from its fit) at a small cost in
angle noise and a worse velocity-step overshoot on a plant twice the
configured inertia.

Bench, 2026-11-07: the same on the real rotor, twelve runs a scenario.
C stays. What the bench adds is a friction reading: T̂_d says the drag
is about 15 mNm and nearly constant with speed, so the configured
b = 7.2e-4 is wrong and T_c = 10 mNm low, and the inertia reads 1.4 to
1.5e-4. Open: whether to re-fit b, T_c and J in the config (C does not
need it, the velocity loop's gains and any feed-forward do), and whether
the sign term wants a deadband for position control at rest. A weight
run still adds the static-load case T̂_d has not seen; the steady-speed
torque balance has covered its calibration.

Bench, 2026-11-08: the T̂_d feed-forward and the Coulomb ramp are in and
do what they were specified for; the 2-per-cycle ripple and the stick-slip
are gone from the velocity. The residual at 0.5 rad/s is a 13 Hz line,
read then as a loop mode of the feed-forward and corrected on 2026-11-09
to the dead-time series. The position-locked torque content, from a scan
of T̂_d over every mechanical order 1 to 400 across both captures and the
open-loop `bench/crawl_sinusoidal.csv` (sensored sinusoidal drive at about
0.9 rad/s, 6.7 turns, no current loop in the path), is a series at the pole
count: 28 per turn (2 per electrical cycle) at 9 to 11 mNm with phase
consistency 1.00 and the same phase against the raw encoder angle on every
power-up and under both drive modes, 56 per turn at 4.5 mNm, 84 per turn
at 2.4 mNm, and a 2-per-turn term at 4 mNm; the slot-and-pole order 168 is
under 0.6 mNm. Present under open-loop voltage drive and locked to the
rotor, that is magnetic cogging at the pole count, the 28 detents a hand
feels on the unpowered motor, and the T̂_d feed-forward cancels it where
T̂_d tracks (fully at crawl, with growing lag above about 5 rad/s). The
next step is a cogging feed-forward: a harmonic at 28 per turn, a second
at 56, referenced to the raw mechanical angle, with the compensated share
carried into the estimator's torque input so T̂_d reads the residual
rather than cancelling the same ripple twice. The SIL load-torque port
(`motor.rs`) now stands in for the weight run.

Bench, 2026-11-09: the compensation is in. Its 28-per-turn term takes
that ripple off the velocity; its 56-per-turn term, fitted from the
open-loop crawl, was four times the motor's and put the same ripple back
at twice the frequency. Re-read from the field-oriented hold's T̂_d (9 mNm,
0.9 mNm), the third capture cancels both to the noise. What is left at
0.5 rad/s is dead-time distortion on an encoder-noise floor, not worth a
compensation at 25 mA of current; low-speed smoothness is called good
here, and the sprint moves to position control.

Should a cogging or other angle-referenced feed-forward ever be added, two
things the code review of that idea established: the estimator's torque
input is the measured K_t·I_q (fw~mc_016), so a compensation current reaches
T̂_d as known input and T̂_d keeps estimating the physical ripple; with k_ff
at 1 the two terms cancel it twice unless the estimator's input also carries
the compensation model. And a term referenced to the electrical angle
inherits each power-up's alignment error times its harmonic order (12× for
the 12-per-cycle order, about ±47° of phase from the ±4° electrical the
0.3-duty alignment parks within against 15 mNm of friction); the raw
mechanical angle θ̂ is alignment-free and is what the captures record.

## Files

- `README.md` — this plan.
- `estimator_order.ipynb` — the analysis, over `sil/` and `bench/`.
- `build_design.sh` — the A, B and Bc comparison firmware, rewritten from the head.
- `run_scenarios.sh` — the SIL captures.
- `sil/` — SIL captures and their provenance, regenerable, not committed.
  `bench/` — bench captures, committed.
