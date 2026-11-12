//! The field-oriented position method (fw~mc_029) against the plant: a 2 pi move
//! on the board's gains lands the rotor on the point without overshoot, and a load
//! at rest is held. One world lives at a time: a second world in the same process
//! waits on the first.

mod common;

use common::proto::{command, envelope, position_setpoint, F_BOARD_REQUEST};
use common::{f64_at, inject, plant, select_method, tap_button, GRID_US, MODE_FOC_POSITION};
use pcs_bldc_sil::board::{board_with_motor, fault_latched, ALIGN_DWELL_MS, GATE_BRINGUP_MS, MOTOR};
use pcs_bldc_sil::{vid, MotorParams, Sil, SilOptions};
use std::f64::consts::{PI, TAU};

/// The plant's angle unwrapped across the seam, from a start taken at construction.
struct Unwrapped {
    last: f64,
    total: f64,
}

impl Unwrapped {
    fn new(sim: &Sil) -> Self {
        Self { last: plant(sim, "angle"), total: 0.0 }
    }

    fn update(&mut self, sim: &Sil) -> f64 {
        let angle = plant(sim, "angle");
        let mut delta = angle - self.last;
        if delta > PI {
            delta -= TAU;
        } else if delta < -PI {
            delta += TAU;
        }
        self.last = angle;
        self.total += delta;
        self.total
    }
}

/// A board world on a free plant of the estimator's configured inertia, the position
/// method enabled and aligned, at rest with nothing written.
fn position_world() -> Sil {
    let probe = board_with_motor(SilOptions::default().grid_us(GRID_US), 0.0, MotorParams::default()).sim;
    let inertia = f64_at(&probe, "app_rotorEstimation_channelConfig[0].inertia_kgm2");
    drop(probe);
    let motor = MotorParams { j_kg_m2: inertia, ..MotorParams::default() };
    let mut sim = board_with_motor(SilOptions::default().grid_us(GRID_US), 0.0, motor).sim;
    sim.run_for_ms(GATE_BRINGUP_MS);
    select_method(&mut sim, MODE_FOC_POSITION);
    tap_button(&mut sim);
    sim.run_for_ms(ALIGN_DWELL_MS + 300);
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

// [test->fw~mc_029~1]
// [test->fw~mc_031~1]
#[test]
fn a_two_pi_move_lands_on_the_point_without_overshoot() {
    let mut sim = position_world();
    let mut angle = Unwrapped::new(&sim);
    inject(&mut sim, &envelope(1, F_BOARD_REQUEST, &command(MODE_FOC_POSITION, &[position_setpoint(0, TAU as f32, 10.0, 50.0, 0.0)])));

    // the profile ends 828 ms after its write (plus the server's and the player's passes)
    let (_, max_during) = run_tracking(&mut sim, &mut angle, 828 + 100 + 10);
    let (settled, max_after) = run_tracking(&mut sim, &mut angle, 500);
    let max = max_during.max(max_after);
    println!("position_method: 2 pi move settled at {settled:.4} rad, peak {max:.4} rad");
    assert!(!fault_latched(&sim), "no fault through the move");
    assert!((settled - TAU).abs() < 0.02, "the rotor is within 0.02 rad of 2 pi from 100 ms after the profile's end: {settled:.4}");
    assert!(max <= TAU + 0.05, "the rotor never passes 2 pi + 0.05 rad: peak {max:.4}");
}

// [test->fw~mc_029~1]
#[test]
fn a_load_at_rest_is_held_within_the_spec() {
    let mut sim = position_world();
    let mut angle = Unwrapped::new(&sim);
    inject(&mut sim, &envelope(1, F_BOARD_REQUEST, &command(MODE_FOC_POSITION, &[position_setpoint(0, 1.0, 10.0, 50.0, 0.0)])));
    let (rest, _) = run_tracking(&mut sim, &mut angle, 1500);
    assert!((rest - 1.0).abs() < 0.02, "at rest on the setpoint before the load: {rest:.4}");

    sim.write(&vid(MOTOR, "load_torque"), 0.010).expect("apply the load");
    let mut excursion = 0.0f64;
    let mut held = rest;
    for _ in 0..(1000 * 1000 / GRID_US) {
        sim.run_for_us(GRID_US);
        held = angle.update(&sim);
        excursion = excursion.max((held - rest).abs());
    }
    println!("position_method: 10 mNm load moved the rotor {excursion:.4} rad at most, {:.4} rad off after 1 s", held - 1.0);
    assert!(!fault_latched(&sim), "no fault under the load");
    assert!(excursion < 0.05, "the load moves the rotor less than 0.05 rad: {excursion:.4}");
    assert!((held - 1.0).abs() < 0.01, "back within 0.01 rad of the setpoint 1 s after the load: {held:.4}");
}
