//! The field-oriented methods' large-signal behaviour on the free plant: reversals
//! stay under the phase trip, a velocity step settles, the integrals hold under
//! the voltage limit, a position step on the velocity clamp lands, the position
//! target moves smoothly between 1 ms passes, and the first alignment hands the
//! drive no disturbance kick. Every number is the firmware's or the plant's,
//! sampled every 50 us PWM cycle.

mod common;

use common::proto::{command, envelope, position_setpoint, speed_setpoint, stop_setpoint, torque_setpoint};
use common::{
    bool_at, enabled_world, f64_at, inject, mc, mc_config, plant, select_method, tap_button, Unwrapped, EST, GRID_US,
    MODE_FOC_POSITION, MODE_FOC_TORQUE, MODE_FOC_VELOCITY,
};
use pcs_bldc_sil::board::{align_dwell_ms, board_with, fault_latched, phase_trip_a, GATE_BRINGUP_MS};
use pcs_bldc_sil::{MotorParams, Sil};

/// The largest phase-current magnitude the plant carries this cycle.
fn plant_peak_phase_a(sim: &Sil) -> f64 {
    ["phase_current_u", "phase_current_v", "phase_current_w"]
        .iter()
        .map(|p| plant(sim, p).abs())
        .fold(0.0, f64::max)
}

/// Run `ms` cycle by cycle; the largest plant phase current over it.
fn run_peak(sim: &mut Sil, ms: u64) -> f64 {
    let mut peak = 0.0_f64;
    for _ in 0..(ms * 1000 / GRID_US) {
        sim.run_for_us(GRID_US);
        peak = peak.max(plant_peak_phase_a(sim));
    }
    peak
}

// [test->fw~mc_025~1]
// [test->fw~safety_001~1]
#[test]
fn a_full_speed_reversal_stays_under_the_phase_trip() {
    // +18 rad/s held, then -18 at a zero rate: the current regulator holds I_max
    // through the regenerative half of the reversal with the windings' back-EMF
    // at its largest, so the plant's phase currents stay under the trip.
    let mut sim = enabled_world(MODE_FOC_VELOCITY, MotorParams::default());
    let max = mc_config(&sim, "maxVelocity_radPerSec") as f32;
    let sequence = vec![speed_setpoint(0, max, 0.0), speed_setpoint(400_000, -max, 0.0), stop_setpoint(900_000)];
    inject(&mut sim, &envelope(1, command(MODE_FOC_VELOCITY, sequence)));
    let peak = run_peak(&mut sim, 800);
    let trip = phase_trip_a(&sim);
    println!("foc_dynamics: +/-{max} rad/s reversal: plant at {:+.2} rad/s, peak phase current {peak:.3} A (trip {trip} A)", plant(&sim, "velocity"));
    assert!(!fault_latched(&sim), "no fault through the reversal");
    assert!(plant(&sim, "velocity") < -0.9 * f64::from(max), "the shaft reversed");
    assert!(peak < trip, "the plant's phase currents stay under the {trip} A trip: {peak:.3} A");
}

// [test->fw~safety_001~1]
#[test]
fn a_full_torque_reversal_stays_under_the_phase_trip() {
    // +K_t I_max until the shaft runs at its voltage-limited speed, then -K_t I_max
    // at a zero rate: the reversal asks the most current against the most back-EMF.
    let mut sim = enabled_world(MODE_FOC_TORQUE, MotorParams::default());
    let max = (mc_config(&sim, "foc.torqueConstant_NmPerA") as f32) * (mc_config(&sim, "foc.maxCurrent_A") as f32);
    let sequence = vec![torque_setpoint(0, max, 0.0), torque_setpoint(300_000, -max, 0.0), stop_setpoint(500_000)];
    inject(&mut sim, &envelope(1, command(MODE_FOC_TORQUE, sequence)));
    let mut peak = 0.0_f64;
    let mut fastest = 0.0_f64;
    for _ in 0..(450 * 20) {
        sim.run_for_us(GRID_US);
        peak = peak.max(plant_peak_phase_a(&sim));
        fastest = fastest.max(plant(&sim, "velocity").abs());
    }
    let trip = phase_trip_a(&sim);
    println!("foc_dynamics: +/-{max:.3} Nm reversal: top speed {fastest:.2} rad/s, now {:+.2} rad/s, peak phase current {peak:.3} A (trip {trip} A)", plant(&sim, "velocity"));
    assert!(!fault_latched(&sim), "no fault through the reversal");
    assert!(peak < trip, "the plant's phase currents stay under the {trip} A trip: {peak:.3} A");
}

/// The voltage limit as the firmware applies it: V_max, or the bus's linear limit
/// when that is lower.
fn v_lim(sim: &Sil) -> f64 {
    mc_config(sim, "maxVoltage_V").min(mc(sim, "busVoltage") / 3.0_f64.sqrt())
}

// [test->fw~mc_025~1]
#[test]
fn a_velocity_step_settles_with_the_current_back_off_its_peak() {
    // 0 -> 15 rad/s at a zero rate: the first cycle asks K_vp * 15 / K_t (under
    // I_max at the board's gains), I_q comes back off it as the speed arrives, and
    // the speed overshoots under 10 % and holds within 5 % from 150 ms after the step.
    const OMEGA: f64 = 15.0;
    let mut sim = enabled_world(MODE_FOC_VELOCITY, MotorParams::default());
    let first_demand_a = (mc_config(&sim, "foc.velocityKp_NmPerRadPerSec") * OMEGA / mc_config(&sim, "foc.torqueConstant_NmPerA"))
        .min(mc_config(&sim, "foc.maxCurrent_A"));
    inject(&mut sim, &envelope(1, command(MODE_FOC_VELOCITY, vec![speed_setpoint(0, OMEGA as f32, 0.0)])));
    while mc(&sim, "velocitySetpoint_radPerSec") < 0.5 * OMEGA {
        sim.run_for_us(GRID_US);
    }
    let (mut peak, mut worst_after, mut peak_i_q, mut last_i_q) = (0.0_f64, 0.0_f64, 0.0_f64, 0.0);
    for cycle in 0..(600 * 20u32) {
        sim.run_for_us(GRID_US);
        let omega = mc(&sim, "velocityMeasured_radPerSec");
        peak = peak.max(omega);
        if cycle >= 150 * 20 {
            worst_after = worst_after.max((omega - OMEGA).abs());
        }
        last_i_q = mc(&sim, "iQ_a");
        peak_i_q = peak_i_q.max(last_i_q);
    }
    println!(
        "foc_dynamics: 0 -> {OMEGA} rad/s step: peak {peak:.3} rad/s, worst from 150 ms {worst_after:.3} rad/s; \
         I_q peaked {peak_i_q:.3} A (first-cycle demand {first_demand_a:.3} A), now {last_i_q:.3} A"
    );
    // The speed arrives within a few current-loop time constants, so I_q peaks
    // short of the first cycle's demand.
    assert!(peak_i_q > 0.5 * first_demand_a, "the step drives a large share of its proportional demand: {peak_i_q:.3} A");
    assert!(last_i_q < 0.1 * peak_i_q, "I_q comes back off its peak once the speed arrives: {last_i_q:.3} A");
    assert!(peak < 1.10 * OMEGA, "overshoot under 10 %: peak {peak:.3}");
    assert!(worst_after < 0.05 * OMEGA, "within 5 % from 150 ms: {worst_after:.3}");
    assert!(!fault_latched(&sim), "no fault");
}

// [test->fw~mc_025~1]
#[test]
fn the_velocity_integral_holds_under_the_voltage_limit() {
    // Against a 0.15 Nm load the 18 rad/s rating needs more than V_lim (back-EMF
    // plus R times the load current), so the hold sits on the voltage limit short of
    // its target for 300 ms with the speed error standing. Then the target drops to
    // zero: a wound-up integral would keep I_q forward; held, the demand reverses
    // within a few cycles and the shaft comes back to rest.
    const LOAD_NM: f64 = 0.15;
    let mut sim = enabled_world(MODE_FOC_VELOCITY, MotorParams::default());
    sim.write(&pcs_bldc_sil::vid(pcs_bldc_sil::board::MOTOR, "load_torque"), LOAD_NM).expect("load the shaft");
    sim.run_for_ms(100);
    let max = mc_config(&sim, "maxVelocity_radPerSec") as f32;
    inject(&mut sim, &envelope(1, command(MODE_FOC_VELOCITY, vec![speed_setpoint(0, max, 0.0), speed_setpoint(300_000, 0.0, 0.0)])));
    let (mut limited_cycles, mut prev_target) = (0u32, 0.0);
    let (mut dropped_at, mut reversed_after, mut held_speed) = (None, None, 0.0);
    for cycle in 0..(800 * 20u32) {
        sim.run_for_us(GRID_US);
        let applied = mc(&sim, "vAlpha_v").hypot(mc(&sim, "vBeta_v"));
        if applied > 0.98 * v_lim(&sim) {
            limited_cycles += 1;
        }
        let target = mc(&sim, "velocitySetpointRequested_radPerSec");
        if (prev_target > 0.0) && (target == 0.0) && dropped_at.is_none() {
            dropped_at = Some(cycle);
            held_speed = plant(&sim, "velocity");
        }
        prev_target = target;
        if let Some(t) = dropped_at {
            if reversed_after.is_none() && (mc(&sim, "iQ_a") < 0.0) {
                reversed_after = Some(cycle - t);
            }
        }
    }
    println!(
        "foc_dynamics: {max} rad/s against {LOAD_NM} Nm: held at {held_speed:.2} rad/s with {} ms on the voltage limit; \
         I_q reversed {reversed_after:?} cycles after the drop; plant now {:+.3} rad/s",
        limited_cycles / 20,
        plant(&sim, "velocity")
    );
    assert!(limited_cycles > 200 * 20, "the hold rides the voltage limit: {} ms", limited_cycles / 20);
    assert!(held_speed < 0.95 * f64::from(max), "short of its target: {held_speed:.2} rad/s");
    let reversed_after = reversed_after.expect("I_q reverses after the drop");
    assert!(reversed_after <= 2 * 20, "the demand reverses within 2 ms of the drop: {reversed_after} cycles");
    assert!(plant(&sim, "velocity").abs() < 0.5, "the shaft comes back to rest: {:+.3} rad/s", plant(&sim, "velocity"));
    assert!(!fault_latched(&sim), "no fault");
}

// [test->fw~mc_029~1]
#[test]
fn a_position_step_on_the_velocity_clamp_lands_without_overshoot() {
    // A 2 pi setpoint with a zero velocity limit (the maximum velocity) and a zero
    // acceleration limit (a step): the command sits on the velocity clamp for most
    // of the move, its integral held, and the rotor lands within 0.02 rad, never
    // more than 0.05 rad past.
    const TARGET: f64 = std::f64::consts::TAU;
    let mut sim = enabled_world(MODE_FOC_POSITION, MotorParams::default());
    sim.run_for_ms(200);
    let mut angle = Unwrapped::new(&sim);
    inject(&mut sim, &envelope(1, command(MODE_FOC_POSITION, vec![position_setpoint(0, TARGET as f32, 0.0, 0.0, 0.0)])));
    let max_velocity = mc_config(&sim, "maxVelocity_radPerSec");
    let (mut peak, mut clamped_cycles, mut last) = (f64::MIN, 0u32, 0.0);
    for _ in 0..(1500 * 20) {
        sim.run_for_us(GRID_US);
        last = angle.update(&sim);
        peak = peak.max(last);
        if plant(&sim, "velocity") > 0.9 * max_velocity {
            clamped_cycles += 1;
        }
    }
    println!(
        "foc_dynamics: 2 pi step at the clamp: {} ms near {max_velocity} rad/s, settled {:+.4} rad off, peak {:+.4} rad past",
        clamped_cycles / 20,
        last - TARGET,
        peak - TARGET
    );
    assert!(clamped_cycles > 100 * 20 / 10, "the move runs at the velocity clamp");
    assert!((last - TARGET).abs() < 0.02, "lands within 0.02 rad: {:+.4}", last - TARGET);
    assert!(peak <= TARGET + 0.05, "never more than 0.05 rad past: {:+.4}", peak - TARGET);
    assert!(!fault_latched(&sim), "no fault");
}

// [test->fw~mc_031~1]
#[test]
fn the_cycle_position_target_moves_smoothly_between_passes() {
    // During the 10 rad/s cruise of a 2 pi move the cycle's position target
    // advances by about omega T every cycle: no 1 kHz staircase (a pass's whole
    // omega * 1 ms landing on one cycle).
    let mut sim = enabled_world(MODE_FOC_POSITION, MotorParams::default());
    sim.run_for_ms(200);
    inject(&mut sim, &envelope(1, command(MODE_FOC_POSITION, vec![position_setpoint(0, std::f64::consts::TAU as f32, 10.0, 50.0, 0.0)])));
    sim.run_for_ms(300);   // past the 200 ms acceleration
    let step_nominal = 10.0 * GRID_US as f64 * 1e-6;
    let mut last = mc(&sim, "cyclePositionTarget_rad");
    let (mut largest, mut smallest) = (0.0_f64, f64::MAX);
    for _ in 0..(100 * 20) {
        sim.run_for_us(GRID_US);
        let now = mc(&sim, "cyclePositionTarget_rad");
        largest = largest.max(now - last);
        smallest = smallest.min(now - last);
        last = now;
    }
    println!("foc_dynamics: cruise per-cycle target advance {smallest:.6}..{largest:.6} rad against omega T = {step_nominal:.6} rad");
    assert!(largest < 1.5 * step_nominal, "no cycle takes a pass's whole advance: {largest:.6} rad");
    assert!(smallest > 0.5 * step_nominal, "every cycle advances: {smallest:.6} rad");
}

// [test->fw~mc_016~1]
#[test]
fn the_first_alignment_hands_the_drive_no_disturbance_kick() {
    // Before the first offset capture the estimator's torque input is zero, so the
    // alignment current (on an unknown frame) puts nothing into the disturbance
    // state: T_d stays within the friction the shaft really feels through the dwell,
    // and the first cycles of the velocity method at a zero target ask no current.
    let mut sim = board_with(Sil::options().grid_us(GRID_US), 0.0).sim;
    sim.run_for_ms(GATE_BRINGUP_MS);
    select_method(&mut sim, MODE_FOC_VELOCITY);
    tap_button(&mut sim);
    let t_c = MotorParams::default().t_c_nm;
    let cogging: f64 = MotorParams::default().cogging.iter().map(|h| h.amp_nm).sum();
    let mut worst_td = 0.0_f64;
    let mut first_drive = None;
    for _ in 0..((align_dwell_ms(&sim) + 200) * 20) {
        sim.run_for_us(GRID_US);
        let aligned = bool_at(&sim, &format!("{}.isAligned", common::MC));
        if !aligned {
            worst_td = worst_td.max(f64_at(&sim, &format!("{EST}.x_hat.e[2][0]")).abs());
        } else if first_drive.is_none() {
            first_drive = Some(mc(&sim, "iQ_a"));
        }
    }
    let i_q_first = first_drive.expect("the alignment completes");
    println!("foc_dynamics: through the first alignment |T_d| peaked at {:.2} mNm; the first driving cycle's I_q {i_q_first:+.4} A", 1e3 * worst_td);
    assert!(worst_td < t_c + cogging, "T_d stays within the shaft's friction and cogging: {:.2} mNm", 1e3 * worst_td);
    let kick_a = (t_c + cogging) / mc_config(&sim, "foc.torqueConstant_NmPerA");
    assert!(i_q_first.abs() < kick_a, "no feed-forward kick on the first driving cycle: {i_q_first:+.4} A");
    assert!(!fault_latched(&sim), "no fault");
}
