//! The rotor tracking estimator (fw~est_rotor_001 / _002) against the plant:
//! the encoder-corrected angle and velocity the firmware holds, read by DWARF
//! out of `app_rotorEstimation_data`, measured against the plant's true shaft
//! angle and velocity. The board encoder carries its bench-measured noise
//! throughout, so the "noise-free encoder angle" the spec names is the plant
//! angle itself (the encoder is wired forward, no offset).

mod common;

use common::{f64_at, plant, select_method, set_bool, tap_button, GRID_US, MODE_SINUSOIDAL};
use pcs_bldc_sil::board::{board, board_with, fault_latched, ALIGN_DWELL_MS, DIAL, ENCODER, GATE_BRINGUP_MS, MOTOR};
use pcs_bldc_sil::{vid, Board, Sil};
use std::f64::consts::{PI, TAU};
use voyant::vsig_id;

/// The estimator's state vector, `x_hat = [theta, omega]`, channel 0.
const EST: &str = "app_rotorEstimation_data.channels[0]";

fn est_angle(sim: &Sil) -> f64 {
    f64_at(sim, &format!("{EST}.x_hat.e[0][0]"))
}

fn est_velocity(sim: &Sil) -> f64 {
    f64_at(sim, &format!("{EST}.x_hat.e[1][0]"))
}

/// Signed angle error wrapped to (-pi, pi].
fn wrap_err(a: f64, b: f64) -> f64 {
    (a - b + PI).rem_euclid(TAU) - PI
}

/// `HW_SPI_CHANNEL_AS5048_1`: the commutation encoder's SPI channel.
const ENCODER_SPI: usize = 0;

/// A board world spinning steadily under sensored sinusoidal drive at full dial on
/// the 50 us grid: the operating point the tracking checks sample.
fn spinning_world() -> Sil {
    let Board { mut sim, .. } = board_with(Sil::options().grid_us(GRID_US), 0.0);
    sim.run_for_ms(GATE_BRINGUP_MS);
    select_method(&mut sim, MODE_SINUSOIDAL);
    tap_button(&mut sim);
    sim.run_for_ms(ALIGN_DWELL_MS + 100);
    let mut deg = 0.0;
    while deg < 180.0 {
        deg += 10.0;
        sim.write(&vid(DIAL, "angle[deg]"), deg).expect("turn the dial");
        sim.run_for_ms(20);
    }
    sim.run_for_ms(500);
    let velocity = plant(&sim, "velocity");
    assert!(velocity > 5.0, "the shaft spins under sinusoidal drive: {velocity:.2} rad/s");
    assert!(!fault_latched(&sim), "no fault through the spin-up");
    sim
}

// [test->fw~est_rotor_001~1]
#[test]
fn the_first_valid_read_seeds_the_estimate_at_rest() {
    // The shaft starts at 1 rad, at rest: within a few milliseconds the estimate
    // sits on the encoder angle with no velocity beyond the noise.
    let Board { mut sim, .. } = board(1.0);
    sim.run_for_ms(5);
    let err = wrap_err(est_angle(&sim), plant(&sim, "angle"));
    println!("rotor_estimate: seeded {:.4} rad against the plant's {:.4} rad, omega {:+.4} rad/s",
             est_angle(&sim), plant(&sim, "angle"), est_velocity(&sim));
    assert!(err.abs() < 0.01, "the estimate seeds on the encoder angle: {err:+.4} rad off");
    assert!(est_velocity(&sim).abs() < 0.5, "at rest the velocity estimate is noise only: {:+.3} rad/s", est_velocity(&sim));
}

// [test->fw~est_rotor_001~1]
#[test]
fn the_velocity_converges_within_1pct_and_holds_through_wraps() {
    // The encoder angle driven by hand at a constant 21 rad/s with the bridge dark
    // (the shaft -> encoder route suspended): from the first read of the ramp the
    // velocity estimate converges inside 50 ms and holds over the following second,
    // through three wraps of the encoder angle. The bench noise rides on every
    // reading here (the unit test holds the noise-free 1 %), so the per-sample
    // band is the 1 % plus the noise, and the mean carries the 1 %.
    const RATE: f64 = 21.0;
    let Board { mut sim, .. } = board(0.0);
    sim.suspend_route(
        &vsig_id(MOTOR, "angle").expect("valid vsig id"),
        &vsig_id(ENCODER, "angle").expect("valid vsig id"),
    )
    .expect("drive the encoder by hand");
    sim.run_for_ms(5);

    let mut angle = 0.0_f64;
    let mut worst_pct_after_50 = 0.0_f64;
    let mut errs = Vec::new();
    for ms in 1..=1050u32 {
        angle = (angle + (RATE * 0.001)).rem_euclid(TAU);
        sim.write(&vid(ENCODER, "angle"), angle).expect("advance the encoder");
        sim.run_for_ms(1);
        if ms >= 50 {
            let err = est_velocity(&sim) - RATE;
            worst_pct_after_50 = worst_pct_after_50.max(100.0 * err.abs() / RATE);
            errs.push(err);
        }
    }
    let mean_pct = 100.0 * (errs.iter().sum::<f64>() / (errs.len() as f64)).abs() / RATE;
    println!("rotor_estimate: ramp at {RATE} rad/s: from 50 ms on, velocity error worst {worst_pct_after_50:.2} %, mean {mean_pct:.3} %, final {:.3} rad/s",
             est_velocity(&sim));
    assert!(worst_pct_after_50 < 2.0, "every sample within 1 % plus the noise band from 50 ms on: worst {worst_pct_after_50:.2} %");
    assert!(mean_pct < 1.0, "the velocity holds the rate within 1 %: mean error {mean_pct:.3} %");
}

// [test->fw~est_rotor_002~1]
// [test->fw~est_rotor_001~1]
#[test]
fn the_estimate_tracks_the_spinning_rotor_every_cycle() {
    // Steady spin: for a full second of PWM cycles the angle estimate stays within
    // the spec's 0.15 deg of the plant's shaft angle, and the velocity estimate
    // holds the plant's velocity, its mean within 1 % and every millisecond sample
    // within the noise band.
    let mut sim = spinning_world();
    let mut worst_deg = 0.0_f64;
    let (mut c, mut s) = (0.0_f64, 0.0_f64);
    let mut vel_err = Vec::new();
    for cycle in 0..20_000u32 {
        sim.step().expect("engine step");
        let err = wrap_err(est_angle(&sim), plant(&sim, "angle"));
        worst_deg = worst_deg.max(err.abs().to_degrees());
        c += err.cos();
        s += err.sin();
        if cycle % 20 == 19 {
            vel_err.push(est_velocity(&sim) - plant(&sim, "velocity"));
        }
    }
    let velocity = plant(&sim, "velocity");
    let mean_deg = s.atan2(c).to_degrees();
    let mean_vel_err = vel_err.iter().sum::<f64>() / (vel_err.len() as f64);
    let worst_vel_err = vel_err.iter().fold(0.0_f64, |w, e| w.max(e.abs()));
    println!(
        "rotor_estimate: spinning at {velocity:.2} rad/s: angle error mean {mean_deg:+.3} deg, worst {worst_deg:.3} deg; \
         velocity error mean {mean_vel_err:+.4} rad/s, worst {worst_vel_err:.3} rad/s over 1 s"
    );
    assert!(worst_deg < 0.15, "every cycle within 0.15 deg of the shaft: worst {worst_deg:.3} deg");
    assert!(mean_vel_err.abs() < 0.01 * velocity, "velocity mean within 1 %: {mean_vel_err:+.4} rad/s");
    assert!(worst_vel_err < 0.03 * velocity, "velocity samples within the noise band: worst {worst_vel_err:.3} rad/s");
    assert!(!fault_latched(&sim), "no fault through the spin");
}

// [test->fw~est_rotor_001~1]
#[test]
fn invalid_reads_coast_the_estimate_on_its_velocity() {
    // A stalled encoder SPI makes every read invalid: for four milliseconds the
    // velocity estimate is held bit for bit, the angle coasts on it and stays on
    // the shaft, and no encoder fault latches (fw~safety_002 needs more than five).
    // Clearing the stall resumes the corrections.
    let mut sim = spinning_world();
    let omega_before = est_velocity(&sim);
    set_bool(&sim, &format!("HW_SPI_data.channels[{ENCODER_SPI}].stall"), true);
    sim.run_for_ms(1);   // the first stalled read lands
    let omega_held = est_velocity(&sim);
    let mut worst_deg = 0.0_f64;
    for _ in 0..60 {     // three milliseconds of cycles
        sim.step().expect("engine step");
        assert_eq!(est_velocity(&sim).to_bits(), omega_held.to_bits(), "the velocity estimate is held through invalid reads");
        worst_deg = worst_deg.max(wrap_err(est_angle(&sim), plant(&sim, "angle")).abs().to_degrees());
    }
    println!("rotor_estimate: stalled 4 ms at {omega_before:.2} rad/s: velocity held at {omega_held:.3}, worst angle error {worst_deg:.3} deg");
    assert!(worst_deg < 0.3, "the angle coasts on the held velocity: worst {worst_deg:.3} deg");
    assert!(!fault_latched(&sim), "four invalid reads do not latch the encoder fault");

    set_bool(&sim, &format!("HW_SPI_data.channels[{ENCODER_SPI}].stall"), false);
    sim.run_for_ms(50);
    let err = wrap_err(est_angle(&sim), plant(&sim, "angle")).abs().to_degrees();
    assert!(err < 0.15, "tracking resumes once reads are valid: {err:.3} deg");
    assert!(!fault_latched(&sim), "no fault after the stall clears");
}
