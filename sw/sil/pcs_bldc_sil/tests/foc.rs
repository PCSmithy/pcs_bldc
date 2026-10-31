//! The field-oriented method on the plant with the shaft held: the current loop's
//! step response (fw~mc_023) and the voltage limit under a target the windings cannot
//! carry (fw~mc_024). Every reading is the firmware's own, off the 50 us grid.

use pcs_bldc_sil::board::{
    board_with_motor, fault_latched, port, ALIGN_DWELL_MS, DIAL, GATE_BRINGUP_MS, VBUS_V,
};
use pcs_bldc_sil::{vid, MotorParams, Sil, SilOptions};

mod common;
use common::{bool_at, f64_at, select_method, tap_button, GRID_US, MODE_FOC};

/// `app_motorControl_channels.c`: the FOC block the firmware runs with.
const I_MAX_A: f64 = 1.0;
const V_MAX_V: f64 = 8.0;
const BANDWIDTH_RAD_S: f64 = 1000.0;
/// `APP_USERCONTROLS_DIAL_ACCUM_RANGE_DEG`: dial travel that maps to full-scale command.
const DIAL_FULL_SCALE_DEG: f64 = 180.0;
/// `APP_MOTORCONTROL_MAX_VELOCITY_RAD_PER_SEC`: the full-scale speed target.
const MAX_VELOCITY_RAD_S: f64 = 200.0 * std::f64::consts::TAU / 60.0;   // 200 rpm

const MC: &str = "app_motorControl_data.channels[0]";

fn mc(sim: &Sil, field: &str) -> f64 {
    f64_at(sim, &format!("{MC}.{field}"))
}

/// The board with the rotor clamped at electrical zero, FOC selected, enabled and aligned.
fn held_rotor_in_foc() -> Sil {
    let motor = MotorParams { rotor_locked: true, ..MotorParams::default() };
    let mut sim = board_with_motor(SilOptions::default().grid_us(GRID_US), 0.0, motor).sim;
    sim.run_for_ms(GATE_BRINGUP_MS);
    select_method(&mut sim, MODE_FOC);
    tap_button(&mut sim);
    sim.run_for_ms(ALIGN_DWELL_MS + 100);
    assert!(bool_at(&sim, &format!("{MC}.isAligned")), "aligned with the shaft held");
    assert!(!fault_latched(&sim), "no fault through the enable");
    sim
}

/// Turn the dial to `fraction` of full scale and run, tick by tick, until the step
/// picks the new target up (the dial reaches the firmware over its own read cadence,
/// tens of ms); the tick this returns on is the method's first on the new target.
fn step_dial_to(sim: &mut Sil, fraction: f64) {
    sim.write(&vid(DIAL, "angle[deg]"), fraction * DIAL_FULL_SCALE_DEG)
        .expect("turn the dial");
    let target = fraction * MAX_VELOCITY_RAD_S;
    for _ in 0..(500_000 / GRID_US) {
        sim.run_for_us(GRID_US);
        if mc(sim, "velocitySetpointCurrent_radPerSec") > (0.5 * target) {
            return;
        }
    }
    panic!(
        "the dial turn never reached the step: requested {} rad/s, dial command {}, dial raw {} deg",
        mc(sim, "velocitySetpointRequested_radPerSec"),
        f64_at(sim, "app_userControls_data.dialCommand"),
        f64_at(sim, "app_userControls_data.dialAngleRaw_deg")
    );
}

/// The stator vector the bridge is applying, recovered from the three duties at the
/// bus voltage the firmware published: the zero sequence cancels in the differences.
fn applied_vector_v(sim: &Sil) -> f64 {
    let v_bus = mc(sim, "busVoltage");
    let (du, dv, dw) = (port(sim, "PWM_U"), port(sim, "PWM_V"), port(sim, "PWM_W"));
    let v_alpha = v_bus * ((2.0 * du) - dv - dw) / 3.0;
    let v_beta = v_bus * (dv - dw) / 3.0_f64.sqrt();
    (v_alpha.powi(2) + v_beta.powi(2)).sqrt()
}

// [test->fw~mc_023~1]
#[test]
fn a_current_step_into_the_held_rotor_rises_at_the_designed_bandwidth() {
    // Half scale: i_q* = 0.5 A, K_p step 2.5 V under the 8 V limit. The design is a
    // first-order rise at w_bw: 63 % at 1/w_bw, inside the 2/w_bw the spec allows.
    let step_a = 0.5 * I_MAX_A;
    let tau_us = (1.0e6 / BANDWIDTH_RAD_S) as u64;
    let mut sim = held_rotor_in_foc();
    assert!(mc(&sim, "iQ_a").abs() < 0.02, "no current before the step");

    step_dial_to(&mut sim, 0.5);
    let mut i_q = Vec::new();
    for _ in 0..((5 * tau_us) / GRID_US) {
        sim.run_for_us(GRID_US);
        i_q.push(mc(&sim, "iQ_a"));
    }
    let rise_us = i_q
        .iter()
        .position(|&i| i >= 0.63 * step_a)
        .map(|k| ((k as u64) + 1) * GRID_US);
    let peak = i_q.iter().cloned().fold(f64::MIN, f64::max);
    let settled = *i_q.last().unwrap();
    let i_d = mc(&sim, "iD_a");
    println!(
        "foc: {step_a} A step -> 63 % at {rise_us:?} us (2/w_bw = {} us), peak {peak:.4} A, \
         settled {settled:.4} A, I_d {i_d:+.4} A",
        2 * tau_us
    );

    let rise_us = rise_us.expect("I_q reaches 63 % of the step");
    assert!(rise_us <= 2 * tau_us, "63 % inside 2/w_bw: {rise_us} us");
    assert!(peak <= 1.10 * step_a, "overshoot under 10 % of the step: peak {peak:.4} A");
    assert!(((settled - step_a) / step_a).abs() < 0.05, "settles on the target: {settled:.4} A");
    assert!(i_d.abs() < 0.05, "I_d stays at its zero target: {i_d:.4} A");
    assert!(!fault_latched(&sim), "no fault through the step");
}

// [test->fw~mc_024~1]
#[test]
fn a_target_the_windings_cannot_carry_holds_the_vector_at_the_limit() {
    // Full scale: i_q* = 1 A asks 14.7 V of the 8 V limit. From 10 ms on the applied
    // vector sits at V_lim as the firmware sees it, and the current is what that vector
    // drives through R at the plant's bus.
    let v_lim = V_MAX_V.min(VBUS_V / 3.0_f64.sqrt());
    let r_ohm = MotorParams::default().r_ohm;
    let mut sim = held_rotor_in_foc();

    step_dial_to(&mut sim, 1.0);
    sim.run_for_ms(10);
    let mut worst_v = 0.0_f64;
    for _ in 0..(10_000 / GRID_US) {
        sim.run_for_us(GRID_US);
        worst_v = worst_v.max((applied_vector_v(&sim) - v_lim).abs());
    }
    let i_q = mc(&sim, "iQ_a");
    let v_applied = v_lim * VBUS_V / mc(&sim, "busVoltage");
    println!(
        "foc: limit {v_lim:.3} V, worst deviation over 10 ms {worst_v:.4} V; I_q {i_q:.4} A \
         against {v_applied:.3} V applied over R = {:.4} A",
        v_applied / r_ohm
    );

    assert!(worst_v <= 0.02 * v_lim, "the vector holds V_lim within 2 %: off by {worst_v:.4} V");
    assert!(((i_q - (v_applied / r_ohm)) / (v_applied / r_ohm)).abs() < 0.05, "I_q is the applied vector over R: {i_q:.4} A");
    assert!(!fault_latched(&sim), "a limited vector never trips");
}
