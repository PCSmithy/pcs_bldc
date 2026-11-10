//! The field-oriented velocity method (fw~mc_025) and the cogging compensation
//! (fw~mc_028) against the plant: the velocity method holds a speed on a rotor
//! whose cogging the firmware's configured model matches, and the ripple that
//! reaches the shaft is compared with the uncompensated case. One world lives
//! at a time: a second world in the same process waits on the first.

mod common;

use common::proto::{command, envelope, speed_setpoint, F_BOARD_REQUEST};
use common::{f64_at, inject, plant, select_method, tap_button, u64_at, GRID_US, MODE_FOC_VELOCITY};
use pcs_bldc_sil::board::{board_with_motor, fault_latched, ALIGN_DWELL_MS, GATE_BRINGUP_MS};
use pcs_bldc_sil::{CogHarmonic, MotorParams, Sil, SilOptions};

const FOC: &str = "app_motorControl_channelConfig[0].foc";

/// The firmware's configuration this test leans on: the first cogging harmonic,
/// the compensation gain, the disturbance feed-forward gain, the estimator's J.
struct Configured {
    model: CogHarmonic,
    k_cog: f64,
    k_ff: f64,
    inertia: f64,
}

fn configured() -> Configured {
    let sim = board_with_motor(SilOptions::default().grid_us(GRID_US), 0.0, MotorParams::default()).sim;
    Configured {
        model: CogHarmonic {
            amp_nm: f64_at(&sim, &format!("{FOC}.cogging[0].amplitude_Nm")),
            per_rev: u64_at(&sim, &format!("{FOC}.cogging[0].order_perRev")) as u16,
            phase_rad: f64_at(&sim, &format!("{FOC}.cogging[0].phase_rad")),
        },
        k_cog: f64_at(&sim, &format!("{FOC}.coggingKff")),
        k_ff: f64_at(&sim, &format!("{FOC}.velocityKff")),
        inertia: f64_at(&sim, "app_rotorEstimation_channelConfig[0].inertia_kgm2"),
    }
}

/// A board world on a plant of the configured inertia with the given cogging,
/// under the velocity method holding `omega` rad/s, settled 500 ms into the hold.
fn cogging_hold_world(inertia: f64, cogging: CogHarmonic, omega: f32) -> Sil {
    let mut motor = MotorParams { j_kg_m2: inertia, ..MotorParams::default() };
    motor.cogging[0] = cogging;
    let mut sim = board_with_motor(SilOptions::default().grid_us(GRID_US), 0.0, motor).sim;
    sim.run_for_ms(GATE_BRINGUP_MS);
    select_method(&mut sim, MODE_FOC_VELOCITY);
    tap_button(&mut sim);
    sim.run_for_ms(ALIGN_DWELL_MS + 100);
    inject(&mut sim, &envelope(1, F_BOARD_REQUEST, &command(MODE_FOC_VELOCITY, &[speed_setpoint(0, omega, 0.0)])));
    sim.run_for_ms(500);
    sim
}

/// The amplitude of the plant velocity's component at `per_rev` cycles per turn
/// over `ms` of running, by least squares against the plant angle.
fn ripple_amplitude(sim: &mut Sil, per_rev: u16, ms: u32) -> f64 {
    let (mut c, mut s, mut cc, mut ss, mut cs, mut vc, mut vs, mut vsum) = (0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    let n = ms * 20;
    for _ in 0..n {
        sim.run_for_us(GRID_US);
        let a = f64::from(per_rev) * plant(sim, "angle");
        let v = plant(sim, "velocity");
        let (sa, ca) = a.sin_cos();
        c += ca; s += sa; cc += ca * ca; ss += sa * sa; cs += ca * sa; vc += v * ca; vs += v * sa; vsum += v;
    }
    // remove the mean, then solve the 2x2 normal equations for the cos and sin coefficients
    let nf = f64::from(n);
    let (vc, vs) = (vc - vsum * c / nf, vs - vsum * s / nf);
    let (cc, ss, cs) = (cc - c * c / nf, ss - s * s / nf, cs - c * s / nf);
    let det = cc * ss - cs * cs;
    let (a_cos, a_sin) = ((vc * ss - vs * cs) / det, (vs * cc - vc * cs) / det);
    a_cos.hypot(a_sin)
}

/// The hold's ripple at the model's order on a plant with `cogging`.
fn hold_ripple(cfg: &Configured, cogging: CogHarmonic) -> f64 {
    let mut sim = cogging_hold_world(cfg.inertia, cogging, 2.0);
    let ripple = ripple_amplitude(&mut sim, cfg.model.per_rev, 1000);
    assert!(!fault_latched(&sim), "no fault through the hold");
    ripple
}

// [test->fw~mc_028~1]
#[test]
fn the_cogging_compensation_takes_the_modelled_ripple_off_the_shaft() {
    // A plant whose cogging matches the configured model against one at twice
    // the amplitude: with the model compensated, the second carries exactly the
    // residual the first would have uncompensated, so the ratio of their
    // 28-per-turn velocity ripple is the compensation's effect at the shaft.
    let cfg = configured();
    assert!(cfg.model.amp_nm > 0.0 && cfg.k_cog > 0.0, "the board config carries a cogging model to test: amp {} k_cog {}", cfg.model.amp_nm, cfg.k_cog);
    let compensated = hold_ripple(&cfg, cfg.model);
    let uncompensated = hold_ripple(&cfg, CogHarmonic { amp_nm: 2.0 * cfg.model.amp_nm, ..cfg.model });
    println!(
        "velocity_method: cogging {:.1} mNm at {} per turn, k_cog {}, k_ff {}: 28-per-turn velocity ripple {compensated:.4} rad/s compensated against {uncompensated:.4} rad/s uncompensated",
        1e3 * cfg.model.amp_nm, cfg.model.per_rev, cfg.k_cog, cfg.k_ff
    );
    assert!(compensated < 0.2 * uncompensated, "the compensated ripple is under a fifth of the uncompensated: {compensated:.4} vs {uncompensated:.4}");
}
