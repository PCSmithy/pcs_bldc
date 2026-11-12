# Velocity-loop gain sweep

The FOC velocity method (`fw~mc_025`) runs a PI on ω* − ω̂. The textbook
rule K_vp = J·ω_v, K_vi = K_vp·ω_v/10 is the comparison; the configured
gains (K_vp = 0.04, K_vi = 0.5) put the loop near ω_v = K_vp/J = 275 rad/s
at the bench's J = 1.45e-4, under the estimator's 314 rad/s pole pair,
with the integral corner at K_vi/K_vp = 12.5 rad/s. This sweep checks them.

- `sweep.sh [baseline] [w_v ...]` — rewrites the two gains in
  `app_motorControl_channels.c` by the rule for each ω_v (default: the
  configured gains as `baseline`, then 50 60 80 100 rad/s), builds the
  release DLL into `build/native-fw-vsweep`, captures S4 through the SIL
  example (`cargo run --release -p pcs_bldc_sil --example estimator_order
  -- S4_velstep`, see `../estimator_order`) on the 1× plant with
  bench-like friction (Coulomb 15 mNm, viscous 1e-4), and restores the file.
- `analyze.py` — one row per gain set: rise, overshoot, settling, steady
  error, peak torque, and the stop step's undershoot and settling, all on
  the plant's velocity; `sil/velocity_sweep.png` overlays them.
- `sil/` — the captures, local and untracked.

## Step sweep, SIL

Estimator C, the plant at 1.6e-4 kg·m² with the estimator configured to
match, Coulomb 15 mNm and viscous 1e-4, a 10 rad/s step at 0.1 s and the
stop at 0.7 s. Rise is 10 to 90 %, settling is into ±5 % of the step (the
±2 % band is never held: the plant's velocity carries 0.2 to 0.25 rad/s
rms of ripple at every gain, the estimate's noise through the loop).

| Gains | K_vp | K_vi | Rise ms | Overshoot % | Settle ms | Peak Nm | Stop undershoot % |
|---|---|---|---|---|---|---|---|
| configured | 0.040 | 0.50 | 9.5 | 5.2 | 35 | 0.28 | 0.0 |
| ω_v = 50 | 0.0073 | 0.036 | 86 | 3.6 | 275 | 0.06 | 0.0 |
| ω_v = 60 | 0.0087 | 0.052 | 55 | 4.6 | 179 | 0.07 | 0.0 |
| ω_v = 80 | 0.0116 | 0.093 | 34 | 5.4 | 366 | 0.10 | 0.0 |
| ω_v = 100 | 0.0145 | 0.145 | 27 | 5.5 | 240 | 0.12 | 0.0 |
| ω_v = 150 | 0.0218 | 0.326 | 17 | 7.8 | 119 | 0.17 | 0.0 |
| ω_v = 200 | 0.029 | 0.58 | 12 | 10.2 | 567 | 0.22 | 0.4 |
| ω_v = 275 | 0.040 | 1.10 | 8 | 11.0 | 272 | 0.30 | 1.9 |

- The rule's gains are slower at every ω_v and buy nothing in overshoot:
  the integral corner at ω_v/10 makes the response creep to the target
  (the long settling at 80 to 100 rad/s is that creep, not ringing).
- The configured gains are the best of the set: the fastest rise, the
  lowest overshoot above ω_v = 60, settled in 35 ms. On the bench under
  estimator C they overshoot 7.8 ± 0.3 % with no ringing; the ringing
  seen under A and B was the estimator's lag inside the loop.
- At the configured K_vp the rule's K_vi of 1.1 doubles the overshoot and
  undershoots the stop by 2 %: the integral corner wants to sit near
  12.5 rad/s, a twentieth of the proportional bandwidth.
- The cost is torque effort: a 0.28 Nm peak on the step against 0.06 to
  0.12 for the rule's sets, a third of the 1.2 A phase trip at
  K_t = 0.825.

## Crawl sweep, SIL

`sil/kff<k>/S5_crawl_*.csv` and `sil/wdb<ω_db>/S5_crawl_*.csv`: the
0.5 rad/s hold (S5) on the same plant, with the T̂_d feed-forward gain and
the Coulomb ramp width varied. Plant-velocity ripple over the hold:

| Run | k_ff | ω_db | Ripple rms | Spectrum |
|---|---|---|---|---|
| kff0.0 | 0 | 1 | 0.129 rad/s | 1.7 Hz slips, 22 Hz |
| kff0.5 | 0.5 | 1 | 0.113 | broad |
| kff1.0 | 1 | 1 | 0.095 | hump at 8 to 12 Hz, 0.06 rad/s |
| wdb0.3 | 1 | 0.3 | 0.106 | broad |
| wdb3.0 | 1 | 3 | 0.101 | broad |
| wdb10.0 | 1 | 10 | 0.093 | broad |

- The feed-forward lowers the crawl ripple monotonically with k_ff, and the
  10 rad/s step does not depend on it.
- The ramp width is not a lever: 0.3 to 10 rad/s moves the ripple within
  0.09 to 0.11 rad/s with no consistent spectral line.
- The floor of about 0.1 rad/s at every setting is the encoder noise
  through the loop. The bench's 13 Hz line at 0.5 rad/s is inverter
  dead-time distortion (`../estimator_order`), which SIL does not model.

## Decision

K_vp = 0.04 Nm per rad/s, K_vi = 0.5 Nm per rad, k_ff = 1, ω_db = 1 rad/s.
Lower the gains only if the bench shows the torque peak hurting.
