//! The field-oriented method on the plant with the shaft held: the current loop's
//! step response (fw~mc_023) and the voltage limit under a target the windings cannot
//! carry (fw~mc_024). Every reading is the firmware's own, off the 50 us grid, and
//! every rating is the channel config's, read out of the image.

use pcs_bldc_sil::board::{fault_latched, port, DIAL, VBUS_V};
use pcs_bldc_sil::{vid, MotorParams, Sil};

mod common;
use common::{enabled_world, f64_at, mc, mc_config, GRID_US, MODE_FOC_TORQUE};

/// `APP_USERCONTROLS_DIAL_ACCUM_RANGE_DEG`: dial travel that maps to full-scale command.
const DIAL_FULL_SCALE_DEG: f64 = 180.0;

/// The board with the rotor clamped at electrical zero, FOC selected, enabled and aligned.
fn held_rotor_in_foc() -> Sil {
    enabled_world(MODE_FOC_TORQUE, MotorParams { rotor_locked: true, ..MotorParams::default() })
}

/// Turn the dial to `fraction` of full scale and run, tick by tick, until the step
/// picks the new target up (the dial reaches the firmware over its own read cadence,
/// tens of ms); the tick this returns on is the method's first on the new target.
fn step_dial_to(sim: &mut Sil, fraction: f64) {
    sim.write(&vid(DIAL, "angle[deg]"), fraction * DIAL_FULL_SCALE_DEG)
        .expect("turn the dial");
    let target = fraction * mc_config(sim, "maxVelocity_radPerSec");
    for _ in 0..(500_000 / GRID_US) {
        sim.run_for_us(GRID_US);
        if mc(sim, "velocitySetpoint_radPerSec") > (0.5 * target) {
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

/// The stator vector the bridge is applying, (v_alpha, v_beta), recovered from the
/// three duties at the bus voltage the firmware published: the zero sequence cancels
/// in the differences.
fn applied_vector_v(sim: &Sil) -> (f64, f64) {
    let v_bus = mc(sim, "busVoltage");
    let (du, dv, dw) = (port(sim, "PWM_U_duty"), port(sim, "PWM_V_duty"), port(sim, "PWM_W_duty"));
    assert!(du.is_finite() && dv.is_finite() && dw.is_finite() && v_bus.is_finite(), "the duty ports and the bus read");
    (v_bus * ((2.0 * du) - dv - dw) / 3.0, v_bus * (dv - dw) / 3.0_f64.sqrt())
}

// [test->fw~mc_023~1]
#[test]
fn a_current_step_into_the_held_rotor_rises_at_the_designed_bandwidth() {
    // Half scale: i_q* = I_max / 2, a K_p step of L w_bw I_max / 2 (2.5 V) under the
    // 11.5 V limit. The design is a first-order rise at w_bw: 63 % at 1/w_bw, inside
    // the 2/w_bw the spec allows and not before 0.5/w_bw (a loop much faster than
    // designed would be as wrong as a slow one).
    let mut sim = held_rotor_in_foc();
    let step_a = 0.5 * mc_config(&sim, "foc.maxCurrent_A");
    let tau_us = (1.0e6 / mc_config(&sim, "foc.currentLoopBandwidth_radPerSec")) as u64;
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
        "foc: {step_a} A step -> 63 % at {rise_us:?} us (1/w_bw = {tau_us} us), peak {peak:.4} A, \
         settled {settled:.4} A, I_d {i_d:+.4} A"
    );

    let rise_us = rise_us.expect("I_q reaches 63 % of the step");
    assert!(rise_us <= 2 * tau_us, "63 % inside 2/w_bw: {rise_us} us");
    assert!(rise_us >= tau_us / 2, "63 % no sooner than 0.5/w_bw: {rise_us} us");
    assert!(peak <= 1.10 * step_a, "overshoot under 10 % of the step: peak {peak:.4} A");
    assert!(((settled - step_a) / step_a).abs() < 0.05, "settles on the target: {settled:.4} A");
    assert!(i_d.abs() < 0.05, "I_d stays at its zero target: {i_d:.4} A");
    assert!(!fault_latched(&sim), "no fault through the step");
}

// [test->fw~mc_024~1]
#[test]
fn a_target_the_windings_cannot_carry_holds_the_vector_at_the_limit() {
    // Full scale: i_q* = I_max asks R I_max (14.7 V) of the 11.5 V limit. From 10 ms
    // on the applied vector sits at V_lim as the firmware sees it, and the current is
    // what that vector drives through R at the plant's bus.
    let mut sim = held_rotor_in_foc();
    let v_max = mc_config(&sim, "maxVoltage_V");
    let v_lim = v_max.min(VBUS_V / 3.0_f64.sqrt());
    let r_ohm = MotorParams::default().r_ohm;

    step_dial_to(&mut sim, 1.0);
    sim.run_for_ms(10);
    let (mut worst_v, mut worst_vector) = (0.0_f64, 0.0_f64);
    // The duties a cycle writes reach the outputs at the next update event (the
    // compare preload), so the vector on the bridge is the previous cycle's.
    let commanded = |sim: &Sil| (mc(sim, "vAlpha_v"), mc(sim, "vBeta_v"));
    let mut commanded_prev = commanded(&sim);
    for _ in 0..(10_000 / GRID_US) {
        sim.run_for_us(GRID_US);
        let (v_alpha, v_beta) = applied_vector_v(&sim);
        worst_v = worst_v.max((v_alpha.hypot(v_beta) - v_lim).abs());
        worst_vector = worst_vector.max((v_alpha - commanded_prev.0).hypot(v_beta - commanded_prev.1));
        commanded_prev = commanded(&sim);
    }
    let i_q = mc(&sim, "iQ_a");
    let v_applied = v_lim * VBUS_V / mc(&sim, "busVoltage");
    println!(
        "foc: limit {v_lim:.3} V, worst magnitude deviation over 10 ms {worst_v:.4} V, worst vector error \
         {worst_vector:.4} V; I_q {i_q:.4} A against {v_applied:.3} V applied over R = {:.4} A",
        v_applied / r_ohm
    );

    assert!(worst_v <= 0.02 * v_lim, "the vector holds V_lim within 2 %: off by {worst_v:.4} V");
    assert!(worst_vector <= 0.02 * v_max, "the bridge applies the previous cycle's commanded vector within 2 % of V_max: off by {worst_vector:.4} V");
    assert!(((i_q - (v_applied / r_ohm)) / (v_applied / r_ohm)).abs() < 0.05, "I_q is the applied vector over R: {i_q:.4} A");
    assert!(!fault_latched(&sim), "a limited vector never trips");
}
