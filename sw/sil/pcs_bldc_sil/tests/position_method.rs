//! The field-oriented position method (fw~mc_029) against the plant: a 2 pi move
//! on the board's gains lands the rotor on the point without overshoot, on the
//! estimator's configured inertia and on plants at half and twice it, and a load at
//! rest is held. One world lives at a time: a second world in the same process
//! waits on the first.

mod common;

use common::proto::{command, envelope, position_setpoint};
use common::{configured_inertia, enabled_world_from, f64_at, inject, Unwrapped, EST, GRID_US, MODE_FOC_POSITION};
use pcs_bldc_sil::board::{fault_latched, MOTOR};
use pcs_bldc_sil::{vid, CurrentSenseParams, MotorParams, Sil};
use std::f64::consts::TAU;

/// The move every variant makes: 2 pi at a 10 rad/s limit and 50 rad/s^2, to rest.
const DISTANCE_RAD: f64 = TAU;
const VELOCITY_LIMIT: f64 = 10.0;
const ACCEL_LIMIT: f64 = 50.0;

/// A board world on a free plant at `inertia_factor` times the estimator's
/// configured inertia, the position method enabled and aligned, at rest with
/// nothing written.
fn position_world(inertia_factor: f64) -> Sil {
    let plant = |fw: &voyant::Firmware| MotorParams { j_kg_m2: inertia_factor * configured_inertia(fw), ..MotorParams::default() };
    let mut sim = enabled_world_from(MODE_FOC_POSITION, plant, CurrentSenseParams::default());
    sim.run_for_ms(200);
    sim
}

/// Run `ms`, sampling the unwrapped plant angle every grid step: (final, maximum).
fn run_tracking(sim: &mut Sil, angle: &mut Unwrapped, ms: u64) -> (f64, f64) {
    let mut max = f64::MIN;
    let mut last = 0.0;
    for _ in 0..(ms * 1000 / GRID_US) {
        sim.run_for_us(GRID_US);
        last = angle.update(sim);
        if last > max {
            max = last;
        }
    }
    (last, max)
}

/// The duration of a rest-to-rest profile over `distance` at velocity limit `v`
/// and acceleration `a`: a trapezoid when the cruise is reached, else a triangle.
fn profile_ms(distance: f64, v: f64, a: f64) -> u64 {
    let seconds = if distance >= (v * v / a) { (distance / v) + (v / a) } else { 2.0 * (distance / a).sqrt() };
    (seconds * 1000.0).ceil() as u64
}

/// The 2 pi move on a plant at `inertia_factor` times the configured inertia:
/// within `land_tol` rad of the point from 100 ms after the profile's end, never
/// past it by more than `peak_tol` rad.
fn two_pi_move(inertia_factor: f64, land_tol: f64, peak_tol: f64) {
    let mut sim = position_world(inertia_factor);
    let mut angle = Unwrapped::new(&sim);
    let setpoint = position_setpoint(0, DISTANCE_RAD as f32, VELOCITY_LIMIT as f32, ACCEL_LIMIT as f32, 0.0);
    inject(&mut sim, &envelope(1, command(MODE_FOC_POSITION, vec![setpoint])));

    // The profile starts a pass or two after the inject (the server's, then the player's).
    let profile = profile_ms(DISTANCE_RAD, VELOCITY_LIMIT, ACCEL_LIMIT);
    let (_, max_during) = run_tracking(&mut sim, &mut angle, profile + 100 + 10);
    let (settled, max_after) = run_tracking(&mut sim, &mut angle, 500);
    let max = max_during.max(max_after);
    println!(
        "position_method: J x{inertia_factor}: 2 pi move over a {profile} ms profile settled {:+.4} rad off, peak {:+.4} rad past",
        settled - DISTANCE_RAD,
        max - DISTANCE_RAD
    );
    assert!(!fault_latched(&sim), "no fault through the move");
    assert!((settled - DISTANCE_RAD).abs() < land_tol, "within {land_tol} rad of 2 pi from 100 ms after the profile's end: {settled:.4}");
    assert!(max <= DISTANCE_RAD + peak_tol, "never past 2 pi + {peak_tol} rad: peak {max:.4}");
}

// [test->fw~mc_029~1]
// [test->fw~mc_031~1]
#[test]
fn a_two_pi_move_lands_on_the_point_without_overshoot() {
    two_pi_move(1.0, 0.02, 0.05);
}

// [test->fw~mc_029~1]
#[test]
fn a_two_pi_move_lands_at_half_the_configured_inertia() {
    two_pi_move(0.5, 0.02, 0.05);
}

// [test->fw~mc_029~1]
#[test]
fn a_two_pi_move_lands_at_twice_the_configured_inertia() {
    two_pi_move(2.0, 0.02, 0.05);
}

// [test->fw~mc_029~1]
#[test]
fn a_load_at_rest_is_held_within_the_spec() {
    // A 100 mNm step load, an eighth of K_t * I_max and well past what the plant's
    // friction holds: the disturbance state takes it up and the position loop
    // holds the rotor to the spec's bounds.
    const LOAD_NM: f64 = 0.100;
    let mut sim = position_world(1.0);
    let mut angle = Unwrapped::new(&sim);
    inject(&mut sim, &envelope(1, command(MODE_FOC_POSITION, vec![position_setpoint(0, 1.0, 10.0, 50.0, 0.0)])));
    let (rest, _) = run_tracking(&mut sim, &mut angle, 1500);
    assert!((rest - 1.0).abs() < 0.02, "at rest on the setpoint before the load: {rest:.4}");
    let td_before = f64_at(&sim, &format!("{EST}.x_hat.e[2][0]"));

    sim.write(&vid(MOTOR, "load_torque"), LOAD_NM).expect("apply the load");
    let mut excursion = 0.0f64;
    let mut held = rest;
    for _ in 0..(1000 * 1000 / GRID_US) {
        sim.run_for_us(GRID_US);
        held = angle.update(&sim);
        excursion = excursion.max((held - rest).abs());
    }
    let td_shift = f64_at(&sim, &format!("{EST}.x_hat.e[2][0]")) - td_before;
    println!(
        "position_method: {:.0} mNm load moved the rotor {excursion:.4} rad at most, {:+.4} rad off after 1 s; T_d moved {:+.1} mNm",
        1e3 * LOAD_NM,
        held - 1.0,
        1e3 * td_shift
    );
    assert!(!fault_latched(&sim), "no fault under the load");
    assert!(excursion < 0.05, "the load moves the rotor less than 0.05 rad: {excursion:.4}");
    assert!((held - 1.0).abs() < 0.01, "back within 0.01 rad of the setpoint 1 s after the load: {held:.4}");
    // At rest the plant's stiction holds a share within +/- T_c that the
    // estimator cannot see, before the load and after it: the shift in T_d is
    // the load, give or take 2 T_c.
    let t_c = MotorParams::default().t_c_nm;
    assert!(
        (td_shift + LOAD_NM).abs() <= 2.0 * t_c,
        "the disturbance state takes up the load, within the stiction's share: T_d moved {:+.1} mNm",
        1e3 * td_shift
    );
}
