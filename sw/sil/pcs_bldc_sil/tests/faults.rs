//! Fault latches under the field-oriented drive on the board world: a software
//! overcurrent trip (fw~safety_001) and a hardware break (fw~safety_003) take the
//! bridge off and latch, the fault-clear command leaves the drive disabled until
//! a new enable, and a fault ends a running host sequence (fw~ops_002).

mod common;

use common::proto::{clear_fault, command, speed_setpoint, torque_setpoint};
use common::{
    accepted, bool_at, exchange, f64_at, inject, select_method, set_bool, tap_button, Host, GRID_US, MC, MODE_FOC_TORQUE,
    MODE_FOC_VELOCITY,
};
use pcs_bldc_sil::board::{align_dwell_ms, board_with_motor, fault_latched, phase_trip_a, port, Board, GATE_BRINGUP_MS, SENSE};
use pcs_bldc_sil::{vid, MotorParams, Sil, SilOptions};

/// The PWM timer's master output enable, the static a break clears.
const MOE_LATCH: &str = "HW_TIM_data.peripheralData[0].mainOutputEnabled";

fn bridge_enabled(sim: &Sil) -> bool {
    bool_at(sim, &format!("{MC}.bridgeEnabled"))
}

/// The board world on the 50 us grid with `mode` enabled by a tap and aligned.
fn driving(mode: common::Mode, motor: MotorParams) -> Board {
    let mut board = board_with_motor(SilOptions::default().grid_us(GRID_US), 0.0, motor);
    let sim = &mut board.sim;
    sim.run_for_ms(GATE_BRINGUP_MS);
    select_method(sim, mode);
    tap_button(sim);
    sim.run_for_ms(align_dwell_ms(sim) + 100);
    assert!(bool_at(sim, &format!("{MC}.isAligned")), "aligned");
    assert!(bridge_enabled(sim) && !fault_latched(sim), "driving, no fault");
    board
}

/// Clear the fault over the host, then show the drive stays dark until a new
/// enable request, which brings it back.
fn clear_and_reenable(sim: &mut Sil, mode: common::Mode, setpoint: common::proto::board::Setpoint) {
    let (reply, _) = exchange(sim, 90, clear_fault());
    assert!(accepted(&reply), "ClearFault is accepted");
    sim.run_for_ms(50);
    assert!(!fault_latched(sim), "the fault is cleared");
    assert!(!bridge_enabled(sim), "the bridge stays disabled after the clear");
    let (reply, _) = exchange(sim, 91, command(mode, vec![setpoint]));
    assert!(accepted(&reply), "a new enable request is accepted");
    sim.run_for_ms(50);
    assert!(bridge_enabled(sim), "the new enable request drives the bridge again");
    assert!(!fault_latched(sim), "no fault on the re-enable");
}

// [test->fw~safety_001~1]
#[test]
fn an_overcurrent_under_the_torque_method_trips_and_stays_dark_through_the_clear() {
    // The torque method drives a held rotor at half scale; a phase reading past the
    // trip, put on the sense input for one millisecond, latches the fault and takes
    // the bridge off, and the cleared fault leaves it off until a new enable.
    let Board { mut sim, sense, .. } = driving(MODE_FOC_TORQUE, MotorParams { rotor_locked: true, ..MotorParams::default() });
    let (reply, _) = exchange(&mut sim, 1, command(MODE_FOC_TORQUE, vec![torque_setpoint(0, 0.4, 0.0)]));
    assert!(accepted(&reply), "the torque target is accepted");
    sim.run_for_ms(20);
    let trip = phase_trip_a(&sim);

    let (src, dst) = &sense.routes()[0];
    sim.suspend_route(src, dst).expect("drive the U sense input by hand");
    sim.write(&vid(SENSE, "i_u"), 1.25 * trip).expect("a phase past the trip");
    let mut tripped_after_us = None;
    for k in 1..=(2 * 20) {
        sim.step().expect("engine step");
        if fault_latched(&sim) {
            tripped_after_us = Some(k * GRID_US);
            break;
        }
    }
    sim.resume_route(src, dst).expect("the plant drives the sense again");
    sim.run_for_ms(5);
    println!("faults: {:.2} A on U latched the overcurrent fault after {tripped_after_us:?} us", 1.25 * trip);
    let tripped_after_us = tripped_after_us.expect("the fault latches");
    assert!(tripped_after_us <= 1050, "within one control cycle: {tripped_after_us} us");
    assert!(!bridge_enabled(&sim), "the trip takes the bridge off");
    assert_eq!(port(&sim, "TIM1_MOE"), 0.0, "the master output enable is off");
    sim.run_for_ms(50);
    assert!(fault_latched(&sim), "the latch holds with the current gone");

    clear_and_reenable(&mut sim, MODE_FOC_TORQUE, torque_setpoint(0, 0.0, 0.0));
}

// [test->fw~safety_003~1]
#[test]
fn a_break_holds_the_bridge_off_and_latches_a_fault() {
    // The sim TIM models a break as its master-output-enable latch clearing under
    // a driving bridge: the firmware holds the bridge off from that cycle, latches
    // the fault within a millisecond, and nothing re-asserts the output after the
    // break is gone until the fault is cleared and a new enable requested.
    let Board { mut sim, .. } = driving(MODE_FOC_VELOCITY, MotorParams::default());
    let (reply, _) = exchange(&mut sim, 1, command(MODE_FOC_VELOCITY, vec![speed_setpoint(0, 5.0, 0.0)]));
    assert!(accepted(&reply), "the speed target is accepted");
    sim.run_for_ms(200);

    set_bool(&sim, MOE_LATCH, false);
    let mut latched_after_us = None;
    for k in 1..=(20 * 20) {
        sim.step().expect("engine step");
        assert!(!bool_at(&sim, MOE_LATCH), "step {k}: the output enable stays deasserted");
        if latched_after_us.is_none() && fault_latched(&sim) {
            latched_after_us = Some(k * GRID_US);
        }
    }
    println!("faults: a break under a 5 rad/s drive latched the fault after {latched_after_us:?} us");
    let latched_after_us = latched_after_us.expect("the break latches a fault");
    assert!(latched_after_us <= 1050, "within a millisecond: {latched_after_us} us");
    assert!(!bridge_enabled(&sim), "the bridge is off");

    clear_and_reenable(&mut sim, MODE_FOC_VELOCITY, speed_setpoint(0, 0.0, 0.0));
}

// [test->fw~ops_002~1]
#[test]
fn a_fault_mid_sequence_drops_the_rest_through_the_clear() {
    // Speeds at 0 and 300 ms; a break at 150 ms latches a fault, cleared at 200 ms:
    // the 300 ms setpoint is never written and the sequence has ended.
    let Board { mut sim, .. } = driving(MODE_FOC_VELOCITY, MotorParams::default());
    let requested = format!("{MC}.velocitySetpointRequested_radPerSec");
    inject(&mut sim, &common::proto::envelope(1, command(MODE_FOC_VELOCITY, vec![speed_setpoint(0, 2.0, 0.0), speed_setpoint(300_000, 4.0, 0.0)])));
    let mut host = Host::new();
    let mut accepted_at = None;
    for ms in 1..=500u32 {
        sim.run_for_ms(1);
        if accepted_at.is_none() {
            if let Some(reply) = host.reply_to(&mut sim, 1) {
                assert!(accepted(&reply), "the sequence is accepted");
                accepted_at = Some(ms);
            }
        }
        let since = accepted_at.map_or(0, |t| ms - t);
        if since == 150 {
            set_bool(&sim, MOE_LATCH, false);
        }
        if since == 200 {
            assert!(fault_latched(&sim), "the break latched");
            inject(&mut sim, &common::proto::envelope(2, clear_fault()));
        }
    }
    println!("faults: after the fault and its clear the speed request is {} rad/s", f64_at(&sim, &requested));
    assert_eq!(f64_at(&sim, &requested), 2.0, "the setpoint after the fault is never written");
    assert!(!bool_at(&sim, "app_sequencer_data.channels[0].running"), "the sequence has ended");
    assert!(!fault_latched(&sim), "the fault was cleared");
    assert!(!bridge_enabled(&sim), "and the drive stays dark");
}
