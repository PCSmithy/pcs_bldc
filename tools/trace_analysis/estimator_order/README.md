# Estimator order characterisation

The test plan, analysis and decision behind the rotor estimator's order:
a second-order tracking observer (A), the same with a known-input term
(B), or a third order with an unmodelled-torque state (C). The same
sequences run in SIL and on the bench, and one notebook reads both. C is
the estimator (`fw~est_rotor_001/002`, `specs/firmware/est/rotor.md`;
derivation in `docs/references/rotor-observer-derivation.md`).

## Designs

| Design | What |
|--------|------|
| A | Second-order tracking observer, constant-velocity prediction |
| B | A plus the known-input term G·u, u = (K_t·I_q − b·ω̂)/J |
| C | B plus a state T̂_d for the unmodelled torque, and the Coulomb term T_c·sat(ω̂/ω_db) in u |

A lags under acceleration: angle a/K_i, velocity 2ζa/ω_n, a time lag of
2ζ/ω_n = 6.4 ms at ω_n = 2π·50, ζ = 1. B removes it when K_t/J and b are
right and the load is the model's. What the model cannot name (a wrong J,
Coulomb friction, a hand on the shaft) B leaves as a correction residual;
C absorbs its steady part in T̂_d. B with the Coulomb term (Bc) is C with
T̂_d held at zero. The repo builds C only; the A and B results below came
from comparison builds of the estimator.

## Scenarios

Each scenario is one `Command`: the points below, then the stop at 1000 ms.
Torque runs start from rest, enabled and aligned, under the torque method;
the velocity runs under the velocity method. S2 and S3 stay under the
voltage ceiling (about 18 rad/s on the 20 V bus).

| Scenario | Mode | Points (t ms, target, rate limit) | Exercises |
|----------|------|-----------------------------------|-----------|
| `S1_step` | torque | (100, 0.08 Nm, 0), (160, 0.01, 0), (700, 0, 0) | An acceleration step, a slow decay, a coast to rest |
| `S2_ramp` | torque | (100, 0.04 Nm, 0.25 Nm/s), (260, 0.01, 0.25), (800, 0, 0.25) | Ramped acceleration, the input term's tracking |
| `S3_reversal` | torque | (100, 0.03 Nm, 0), (300, −0.03, 0), (600, 0.03, 0), (900, 0, 0) | Reversals through zero speed; separates acceleration from velocity for the residual fit |
| `S4_velstep` | velocity | (100, 10 rad/s, 0), (700, 0, 0) | The estimator inside the velocity loop |
| `S5_crawl` | velocity | (100, 0.5 rad/s, 0) | A crawl: friction ramp band, stiction, cogging |

SIL runs each scenario on four plant variants: the inertia at 0.5×, 1× and
2× the estimator's configured J (read from the firmware under test), and
the Coulomb friction at 4× the plant model's 4.4 mNm. The plant's own
friction (Coulomb 4.4 mNm, viscous 7.2e-4) differs from the estimator's
configured values, so at the 1× variant T̂_d carries the difference by
construction.

## Signals

The DWARF paths are the column names in every capture, SIL or bench.
K_t·I_q is derived from I_q in the notebook.

| Path | Signal | Watch period |
|------|--------|--------------|
| `app_rotorEstimation_data.channels[0].x_hat.e[0][0]` | θ̂, rad | 20 kHz |
| `app_rotorEstimation_data.channels[0].x_hat.e[1][0]` | ω̂, rad/s | 20 kHz |
| `app_rotorEstimation_data.channels[0].x_hat.e[2][0]` | T̂_d, Nm | 20 kHz |
| `app_motorControl_data.channels[0].iQ_a` | I_q, A | 20 kHz |
| `app_rotorEstimation_data.channels[0].angleMeasured_rad` | θ_m, last valid encoder angle | 1 ms |
| `app_rotorEstimation_data.channels[0].angleError_rad` | e = θ_m − θ̂ at the last correction | 1 ms |
| `app_rotorEstimation_data.channels[0].modelledTorque_Nm` | the advance's modelled torque, Nm | 1 ms |
| `app_motorControl_data.channels[0].torqueSetpoint_Nm` | torque target in effect | 1 ms |
| `app_motorControl_data.channels[0].velocitySetpoint_radPerSec` | velocity target in effect | 1 ms |

SIL captures add `plant.angle_rad` and `plant.velocity_radps`, the truth
the bench does not have.

## Running SIL

```bash
tools/trace_analysis/estimator_order/run_scenarios.sh            # every scenario
tools/trace_analysis/estimator_order/run_scenarios.sh S4_velstep # a subset
```

The script builds the release firmware DLL, then runs the SIL example:

```bash
cd sw/sil
PCS_SIL_DLL=<build/native-fw-release/src/libpcs_bldc_fw.dll> PCS_EST_DESIGN=C \
  cargo run --release -p pcs_bldc_sil --example estimator_order -- [scenario ...]
```

Captures land in `sil/` as `<scenario>_<design>_J<factor>[_Tc<factor>].csv`,
one row per PWM cycle, with `provenance_<design>.txt`. `PCS_EST_OUT`
redirects them; `PCS_EST_VARIANTS=nominal` runs the 1× plant alone;
`PCS_EST_PLANT_TC_NM` and `PCS_EST_PLANT_B` replace the plant's friction.

## Bench captures

1. Flash the firmware; point the app's identity gate at its ELF.
2. Watch the nine signals above at the periods in the table.
3. Command widget: FOC torque, 0 Nm, Send; the first enable aligns.
4. Sequence widget: enter the scenario's points under its mode, duration
   1000 ms, save it by name.
5. Run, repeat after the rotor has rested at least 0.5 s (the notebook
   splits a capture at each rest), pause, select from just before the
   first run to 300 ms past the last stop, Export as
   `bench/<scenario>_<design>[_Jcfg<J>]_r<n>.csv`.

`sil/` and `bench/` are local and untracked: the bench captures are the
exports from step 5 on the machine that ran them.

## Metrics

`estimator_order.ipynb` computes, over each run's active window:

- SIL: angle and velocity error against the plant (signed mean, peak, RMS),
  and the velocity lag.
- SIL and bench: the correction residual e (signed mean, peak, RMS, tail
  mean); ω̂ against the encoder's own Δθ/Δt.
- The residual decomposition e ≈ c_a·a + c_ω·ω̂ + c_c·sgn(ω̂) + c_0, which
  reads the input-term fraction (hence the inertia), the encoder delay and
  the load terms; and, for C, the inertia and friction read from T̂_d.

Signed means separate the designs: the per-cycle peaks sit on the encoder
noise (±0.1°, RMS 0.04°).

## Results

SIL, 1× plant unless stated:

| | A | B | C |
|---|---:|---:|---:|
| S1 velocity lag against the plant | 6.3 ms | −0.2 ms | ≈ 0 |
| S1 velocity lag, 0.5× plant | 6.2 ms | 3.0 ms | 1.4 ms |
| S1 velocity lag, 2× plant | — | −6.2 ms | −0.25 ms |
| Residual tail | — | −0.005 to −0.03° | 0.000–0.001° |
| S4 overshoot | 61 % | 7.8 % | 5.1 % |

Bench:

| | A | B (J 1.6e-4) | C |
|---|---:|---:|---:|
| S1 signed residual under torque | 0.101° | — | — |
| S3 signed residual under torque | 0.061° | −0.001° | — |
| S3 ω̂ lag against the encoder | 6 ms | 0 ms | — |
| S4 overshoot | 71 % | 10 % | 7.8 % |
| Residual tail (S4, drag) | — | −0.035° | 0.000° |
| ω̂ bias against the encoder | — | 0.4–0.6 rad/s | 0.04–0.18 rad/s |

The encoder-delay term reads within ±40 µs of zero on every torque run, so
no latency correction enters the inertia reading.

## Decision

C. B removes A's lag once J is right, but leaves the load it cannot name as
a velocity-estimate bias of about 0.45 rad/s that flips with direction;
position control on an unknown load cannot carry that. C removes the
steady residual and the bias in SIL and on the bench, tolerates a wrong J,
and reads the load directly. Its costs: a slightly higher angle RMS
(0.043° against 0.036°), a 0.26° residual peak while T̂_d swings through a
reversal, and more standstill ω̂ noise, which the Coulomb ramp
(`fw~est_rotor_002`, ω_db = 1 rad/s) keeps down.

What the bench reads through C, and the config takes:

- **Inertia** 1.45e-4 kg·m² (S3 fit 1.4–1.5e-4; B's residual fit 1.44e-4).
- **Friction** Coulomb-like at about 15 mNm, flat within the scatter from
  1 to 17 rad/s, with little viscous part: T_c = 15 mNm, b = 1e-4. The
  delivered torque at steady speed matches the drag so read within 1–2 mNm.
- **Cogging** at the pole count, locked to the raw mechanical angle and
  present under open-loop drive: 28 per turn at 9 mNm (phase 1.1 rad),
  56 per turn at 0.9 mNm (−1.75 rad), read from the field-oriented hold's
  T̂_d. The open-loop crawl over-reads the 56-per-turn term four-fold (the
  drive's own ripple lands on the same order). `fw~mc_028` cancels both to
  the noise, and its share joins the estimator's torque input (`fw~mc_016`)
  so T̂_d reads the residual.
- **T̂_d feed-forward** (`fw~mc_025`, k_ff = 1): the 2-per-electrical-cycle
  velocity ripple at 0.5 rad/s falls 0.20 → 0.06 rad/s, stick-slip at
  0.1 rad/s from 17 % to 2 % of the time.

What remains at 0.5 rad/s, about 0.11 rad/s RMS, is a 6/12/24-per-electrical-
cycle line, inverter dead-time distortion, on the encoder-noise floor
through the loop; at 25 mA of I_q a compensation sits inside the
current-sense noise, so low-speed smoothness stands here.

## Files

- `README.md` — this plan and its decision.
- `estimator_order.ipynb` — the analysis, over `sil/` and `bench/`.
- `run_scenarios.sh` — the SIL captures.
- `sil/`, `bench/` — local captures, untracked.
