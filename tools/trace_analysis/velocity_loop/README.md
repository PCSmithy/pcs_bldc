# Velocity-loop gain sweep

The FOC velocity method (`fw~mc_025`) runs a PI on ω* − ω̂ with
K_vp = J·ω_v and K_vi = K_vp·ω_v/10 by derivation. The configured gains
(K_vp = 0.04, K_vi = 0.5) never matched it: at the bench's J = 1.45e-4 they
put the loop near 275 rad/s, under the estimator's 314 rad/s pole pair,
which is what rang on the estimator-order S4 runs. This sweep picks ω_v.

- `sweep.sh [baseline] [w_v ...]` — rewrites the two gains in
  `app_motorControl_channels.c` for each ω_v (default: the configured gains
  as `baseline`, then 50 60 80 100 rad/s),
  builds the release DLL into `build/native-fw-vsweep`, captures S4 through
  `sw/sil/pcs_bldc_sil/tests/estimator_order.rs` on the 1× plant with
  bench-like friction (Coulomb 15 mNm, viscous 1e-4, from the design-C bench
  reading in `../estimator_order`), and restores the file.
- `analyze.py` — one row per gain set: rise, overshoot, settling into ±2 %,
  steady error, peak torque, and the stop step's undershoot and settling,
  all on the plant's velocity; `sil/velocity_sweep.png` overlays them.
- `sil/` — the captures, regenerable, not committed.

## Findings, SIL, 2026-11-07

Design C in the estimator, the plant at the configured 1.6e-4 inertia with
Coulomb 15 mNm and viscous 1e-4, a 10 rad/s step at 0.1 s and the stop at
0.7 s. Rise is 10 to 90 %, settling is into ±5 % of the step (the ±2 % band
is never held: the plant's velocity carries 0.2 to 0.25 rad/s rms of ripple
at every gain, the estimate's noise through the loop, not the gains).

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

- The derived gains are slower at every ω_v and buy nothing in overshoot:
  the K_vp = J·ω_v, K_vi = K_vp·ω_v/10 rule puts the integral corner at
  ω_v/10 and the response creeps to the target on it (the long ±5 %
  settling at 80 to 100 rad/s is that creep, not ringing).
- The configured gains are the best of the set: the fastest rise, the
  lowest overshoot above ω_v = 60, and settled in 35 ms. Their former
  ringing on the bench (`../estimator_order`, S4 under A and B) was the
  estimator's lag inside the loop, which C removes; on the bench under C
  the same gains overshoot 7.8 ± 0.3 % with no ringing.
- At the configured K_vp (ω_v = 275 by the rule) the rule's K_vi of 1.1
  doubles the overshoot against the configured 0.5 and undershoots the
  stop by 2 %: the integral corner wants to stay near K_vi/K_vp = 12.5
  rad/s, a twentieth of the proportional bandwidth, not a tenth.
- The cost of the configured gains is torque effort, a 0.28 Nm peak on
  the step against 0.06 to 0.12 for the derived sets, a third of the
  1.2 A phase trip at K_t = 0.825.

### Crawl sweeps, 2026-11-08

`sil/kff<k>/S5_crawl_*.csv` and `sil/wdb<ω_db>/S5_crawl_*.csv`: the 0.5 rad/s
hold (S5) on the same bench-like plant, run from the head with the T̂_d
feed-forward gain and then the Coulomb ramp width rewritten, to chase the
13 Hz ripple the bench shows at 0.5 rad/s with the feed-forward on
(`../estimator_order`, 2026-11-08). Plant-velocity ripple over the hold:

| Run | k_ff | ω_db | Ripple rms | Spectrum |
|---|---|---|---|---|
| kff0.0 | 0 | 1 | 0.129 rad/s | 1.7 Hz slips, 22 Hz |
| kff0.5 | 0.5 | 1 | 0.113 | broad |
| kff1.0 | 1 | 1 | 0.095 | hump at 8 to 12 Hz, 0.06 rad/s |
| wdb0.3 | 1 | 0.3 | 0.106 | broad |
| wdb3.0 | 1 | 3 | 0.101 | broad |
| wdb10.0 | 1 | 10 | 0.093 | broad |

- The feed-forward lowers the crawl ripple monotonically with k_ff and the
  10 rad/s step does not depend on it at all (kff*/S4). At k_ff = 1 a weak
  hump near 10 Hz appears, SIL's echo of the bench's 13 Hz line, at half its
  amplitude.
- The ramp width is not the lever: 0.3 to 10 rad/s moves the ripple within
  0.09 to 0.11 rad/s with no consistent spectral line, so the ramp's slope
  inside the band is not what drives the mode.
- SIL's crawl ripple floor of about 0.1 rad/s at every setting is the
  encoder noise through the loop. The bench's 13 Hz line turned out to be
  inverter dead-time distortion at 12 per electrical cycle
  (`../estimator_order`, 2026-11-09), which SIL does not model; k_ff is
  not its lever.

Decision: keep K_vp = 0.04 and K_vi = 0.5. The config comment's derivation
is wrong about both the inertia and the rule and should say what the gains
are: ω_v ≈ K_vp/J = 275 rad/s at the bench's 1.45e-4, under the
estimator's 314 rad/s pole pair, with the integral corner at 12.5 rad/s.
Lower them only if the bench shows the torque peak hurting.
