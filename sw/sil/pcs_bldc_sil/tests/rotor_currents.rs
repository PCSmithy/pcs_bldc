//! Rotor-frame current observation (fw~mc_016) through the firmware: phase currents
//! written straight into the sense model decode through the injected path and land
//! as I_d / I_q in the active method's frame. The world is aligned at rest under
//! six-step, so the frame angle is the encoder's electrical angle, zero by
//! construction of the alignment capture.
//!
//! With `PCS_SIL_TRACE_DIR` set, each check drops its historian as an MF4 on exit:
//! the phase currents, alpha/beta, and d/q are all mirrored statics.

mod common;

use common::{f64_at, plant, spin_up, tap_button, GRID_US};
use pcs_bldc_sil::board::{board, board_with, fault_latched, ALIGN_DWELL_MS, DIAL, GATE_BRINGUP_MS, SENSE};
use pcs_bldc_sil::{cid, vid, Board, MotorParams, Sil};
use std::f64::consts::TAU;

/// `motorPolePairs` in `app_motorControl_channels.c`.
const POLE_PAIRS: f64 = 14.0;

/// Injected magnitude: well under the 2 A phase trip, with the derived W at half.
const I_A: f64 = 1.2;

/// Decode slack: 12-bit quantization (~8 mA/phase) plus the encoder-noise frame
/// jitter (~1.5 LSB mechanical x 14 pole pairs, ~0.01 A at this magnitude).
const TOL_A: f64 = 0.05;

/// The 1 ms pass and the 20 kHz step both need a few passes after an injection.
const SETTLE_MS: u64 = 10;

/// A board world with the four motor -> sense routes suspended (the sense inputs
/// take direct writes), the gate up, six-step selected, and the alignment captured.
fn aligned_world() -> Sil {
    let Board { mut sim, sense, .. } = board(0.0);
    for (src, dst) in &sense.routes()[..4] {
        sim.suspend_route(src, dst)
            .unwrap_or_else(|e| panic!("suspend {src} -> {dst}: {e}"));
    }
    sim.run_for_ms(GATE_BRINGUP_MS);
    tap_button(&mut sim);
    sim.run_for_ms(ALIGN_DWELL_MS + 100);
    assert!(
        sim.read_bool(&cid("app_motorControl_data.channels[0].isAligned")),
        "aligned at rest before injecting currents"
    );
    sim
}

/// Command the three phase-sense inputs (A) and let the firmware resample.
fn inject(sim: &mut Sil, phase_a: [f64; 3]) {
    for (local, amps) in ["i_u", "i_v", "i_w"].iter().zip(phase_a) {
        sim.write(&vid(SENSE, local), amps)
            .unwrap_or_else(|e| panic!("write {local}: {e}"));
    }
    sim.run_for_ms(SETTLE_MS);
}

/// The firmware's own (alpha, beta, d, q) as the step computed them.
fn observed(sim: &Sil) -> [f64; 4] {
    let at = |f: &str| f64_at(sim, &format!("app_motorControl_data.channels[0].{f}"));
    [at("iAlpha_a"), at("iBeta_a"), at("iD_a"), at("iQ_a")]
}

fn assert_near(got: f64, want: f64, what: &str) {
    assert!(
        (got - want).abs() < TOL_A,
        "{what}: got {got:.4} A, expected {want:.4} A"
    );
}

// [test->fw~mc_016~1]
#[test]
fn current_along_the_frame_axis_reads_as_id() {
    // (I, -I/2, -I/2) is a stator vector on the alpha axis; at frame angle 0 that is
    // pure I_d.
    let mut sim = aligned_world();
    inject(&mut sim, [I_A, -I_A / 2.0, -I_A / 2.0]);

    let [alpha, beta, d, q] = observed(&sim);
    println!("rotor_currents: d-axis inject -> alpha {alpha:.4} beta {beta:.4} d {d:.4} q {q:.4}");
    assert_near(alpha, I_A, "alpha");
    assert_near(beta, 0.0, "beta");
    assert_near(d, I_A, "I_d");
    assert_near(q, 0.0, "I_q");
    assert!(!fault_latched(&sim), "1.2 A is under the trip");
}

// [test->fw~mc_016~1]
#[test]
fn current_in_quadrature_reads_as_iq() {
    // (0, s, -s) with s = sqrt(3)/2 I is a stator vector on the beta axis
    // (beta = (b - c)/sqrt(3) = I); at frame angle 0 that is pure I_q.
    let s = (3f64.sqrt() / 2.0) * I_A;
    let mut sim = aligned_world();
    inject(&mut sim, [0.0, s, -s]);

    let [alpha, beta, d, q] = observed(&sim);
    println!("rotor_currents: q-axis inject -> alpha {alpha:.4} beta {beta:.4} d {d:.4} q {q:.4}");
    assert_near(alpha, 0.0, "alpha");
    assert_near(beta, I_A, "beta");
    assert_near(d, 0.0, "I_d");
    assert_near(q, I_A, "I_q");
    assert!(!fault_latched(&sim), "under the trip");
}

// [test->fw~mc_016~1]
#[test]
fn the_observation_follows_the_cycle_currents() {
    // A change in the injected currents moves d/q at the next passes; the step
    // consumes the cycle's samples, not a stale copy.
    let mut sim = aligned_world();
    inject(&mut sim, [I_A, -I_A / 2.0, -I_A / 2.0]);
    let [.., d1, _] = observed(&sim);
    inject(&mut sim, [-I_A, I_A / 2.0, I_A / 2.0]);
    let [.., d2, q2] = observed(&sim);
    assert_near(d1, I_A, "I_d before");
    assert_near(d2, -I_A, "I_d after the reversal");
    assert_near(q2, 0.0, "I_q stays zero on the alpha axis");
}

// [test->fw~mc_012~1]
// [test->fw~mc_016~1]
#[test]
fn the_firmware_frame_is_the_rotor_frame_while_spinning() {
    // Spinning under six-step: the firmware's electrical angle tracks the plant's rotor
    // angle (alignment put electrical zero on the rotor flux), and the plant's torque is
    // the q-axis current the firmware observes, T = 1.5 * ke * I_q, with I_q positive for
    // a positive spin. The 1 ms angle publish lags the 20 kHz step by up to a sector at
    // this speed, so the torque match is loose; the frame match is sampled at the publish.
    let Board { mut sim, .. } = board(0.0);
    spin_up(&mut sim);
    let ke = MotorParams::default().ke_v_per_mech_rad_s;

    let (mut c, mut s) = (0.0_f64, 0.0_f64);
    let mut torque = Vec::new();
    let mut i_q = Vec::new();
    let mut i_d = Vec::new();
    for _ in 0..1000 {
        sim.run_for_ms(1);
        let theta_fw = f64_at(&sim, "app_motorControl_data.channels[0].magneticAngle_rad");
        let theta_e = (plant(&sim, "angle") * POLE_PAIRS).rem_euclid(TAU);
        c += (theta_fw - theta_e).cos();
        s += (theta_fw - theta_e).sin();
        torque.push(plant(&sim, "torque"));
        i_q.push(f64_at(&sim, "app_motorControl_data.channels[0].iQ_a"));
        i_d.push(f64_at(&sim, "app_motorControl_data.channels[0].iD_a"));
    }
    let frame_err_deg = s.atan2(c).to_degrees();
    let mean = |v: &[f64]| v.iter().sum::<f64>() / (v.len() as f64);
    let (t_mean, q_mean, d_mean) = (mean(&torque), mean(&i_q), mean(&i_d));
    let predicted = 1.5 * ke * q_mean;
    let corr = {
        let (tm, qm) = (t_mean, q_mean);
        let num: f64 = torque.iter().zip(&i_q).map(|(t, q)| (t - tm) * (q - qm)).sum();
        let dt: f64 = torque.iter().map(|t| (t - tm).powi(2)).sum::<f64>().sqrt();
        let dq: f64 = i_q.iter().map(|q| (q - qm).powi(2)).sum::<f64>().sqrt();
        num / (dt * dq)
    };
    println!(
        "rotor_currents: frame error {frame_err_deg:+.2} deg; torque {t_mean:.4} Nm vs 1.5*ke*I_q          {predicted:.4} Nm (corr {corr:.3}); I_d {d_mean:+.3} A, I_q {q_mean:+.3} A, velocity {:.1} rad/s",
        plant(&sim, "velocity")
    );
    assert!(frame_err_deg.abs() < 5.0, "firmware electrical angle tracks the rotor: {frame_err_deg:.2} deg off");
    assert!(q_mean > 0.0, "a positive spin is positive I_q: {q_mean:.3} A");
    assert!(corr > 0.8, "torque follows I_q: corr {corr:.3}");
    assert!(
        ((t_mean - predicted) / t_mean).abs() < 0.35,
        "T = 1.5 ke I_q within the 1 ms-angle slack: {t_mean:.4} vs {predicted:.4} Nm"
    );
    assert!(!fault_latched(&sim), "no fault through the spin");
}

// [test->fw~mc_019~1]
#[test]
fn the_per_cycle_angle_tracks_the_rotor_at_speed() {
    // Full dial under six-step on the 50 us grid, past 300 rad/s electrical: every
    // cycle's angle, read after the step that produced it, is within the spec's 3 deg
    // of the plant's rotor angle, a millisecond after the last encoder sample included.
    let Board { mut sim, .. } = board_with(Sil::options().grid_us(GRID_US), 0.0);
    spin_up(&mut sim);
    let mut deg = 90.0;
    while deg < 180.0 {
        deg += 10.0;
        sim.write(&vid(DIAL, "angle[deg]"), deg).expect("turn the dial");
        sim.run_for_ms(20);
    }
    sim.run_for_ms(500);
    let omega_e = plant(&sim, "velocity") * POLE_PAIRS;
    assert!(omega_e > 300.0, "spinning past 300 rad/s electrical: {omega_e:.0}");

    let mut worst_deg = 0.0_f64;
    let (mut c, mut s_) = (0.0_f64, 0.0_f64);
    for _ in 0..2000 {
        sim.step().expect("engine step");
        let theta_fw = f64_at(&sim, "app_motorControl_data.channels[0].magneticAngle_rad");
        let theta_e = (plant(&sim, "angle") * POLE_PAIRS).rem_euclid(TAU);
        let err = (theta_fw - theta_e + std::f64::consts::PI).rem_euclid(TAU) - std::f64::consts::PI;
        worst_deg = worst_deg.max(err.abs().to_degrees());
        c += err.cos();
        s_ += err.sin();
    }
    let mean_deg = s_.atan2(c).to_degrees();
    println!("rotor_currents: per-cycle angle at {omega_e:.0} rad/s electrical: mean error {mean_deg:+.2} deg, worst {worst_deg:.2} deg over 2000 cycles");
    assert!(worst_deg < 3.0, "every cycle within 3 deg of the rotor: worst {worst_deg:.2} deg");
    assert!(!fault_latched(&sim), "no fault through the spin");
}
