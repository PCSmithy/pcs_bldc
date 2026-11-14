//! Command authority between the on-device controls and the host (sys~ops_002) on
//! the board world: the button stops a drive the host started without writing a
//! target, and a host method change leaves the host's target in effect until the
//! dial actually moves.

mod common;

use common::proto::{command, position_setpoint, speed_setpoint};
use common::{
    accepted, assert_status, exchange, f64_at, tap_button, Status, MC, MC_CONFIG, MODE_FOC_POSITION, MODE_FOC_VELOCITY,
    MODE_OFF,
};
use pcs_bldc_sil::board::{board_with, fault_latched, align_dwell_ms, DIAL, GATE_BRINGUP_MS};
use pcs_bldc_sil::{cid, vid, Sil};

/// `APP_USERCONTROLS_DIAL_ACCUM_RANGE_DEG`: dial travel that maps to full-scale command.
const DIAL_FULL_SCALE_DEG: f64 = 180.0;

/// The motor's `LastWrite::None`: no target written since the last disable.
const NO_WRITE: Status = Status { name: "None", ordinal: 0 };

fn speed_requested(sim: &Sil) -> f64 {
    f64_at(sim, &format!("{MC}.velocitySetpointRequested_radPerSec"))
}

fn position_requested(sim: &Sil) -> f64 {
    f64_at(sim, &format!("{MC}.positionSetpoint.position_rad"))
}

fn bridge_enabled(sim: &Sil) -> bool {
    sim.read_bool(&cid(&format!("{MC}.bridgeEnabled")))
}

/// The board world on the 50 us grid with the host connected and the gate up.
fn connected_board() -> Sil {
    let mut sim = board_with(Sil::options().grid_us(common::GRID_US), 0.0).sim;
    sim.write(&cid("HW_USB_sim_data.connected"), true).expect("connect the host");
    sim.run_for_ms(GATE_BRINGUP_MS);
    sim
}

/// Run `ms` a millisecond at a time; every value the speed request took.
fn speed_requests_over(sim: &mut Sil, ms: u32) -> Vec<f64> {
    let mut seen = vec![speed_requested(sim)];
    for _ in 0..ms {
        sim.run_for_ms(1);
        let now = speed_requested(sim);
        if (now - seen.last().unwrap()).abs() > 1e-6 {
            seen.push(now);
        }
    }
    seen
}

// [test->sys~ops_002~1]
#[test]
fn the_first_tap_stops_a_host_started_drive_without_writing_a_target() {
    let mut sim = connected_board();
    let (reply, _) = exchange(&mut sim, 1, command(MODE_FOC_POSITION, vec![position_setpoint(0, 1.0, 10.0, 50.0, 0.0)]));
    assert!(accepted(&reply), "the host starts the position method");
    sim.run_for_ms(align_dwell_ms(&sim) + 1000);
    assert!(bridge_enabled(&sim), "the host's drive runs");
    let (position, speed) = (position_requested(&sim), speed_requested(&sim));
    assert!((position - 1.0).abs() < 1e-6, "the host's position setpoint stands: {position}");

    // The disable drops the position setpoint (fw~mc_030), so a write after it shows
    // as the motor's last write, which a write while dark sets for the next enable.
    tap_button(&mut sim);
    let speeds = speed_requests_over(&mut sim, 500);
    println!("command_authority: after the tap, bridge {}, position {:.4} rad, speed requests {speeds:?}",
             bridge_enabled(&sim), position_requested(&sim));
    assert!(!bridge_enabled(&sim), "the first tap stops the host's drive");
    assert_status(&sim, &format!("{MC}.lastWrite"), &NO_WRITE, "the tap writes no position");
    assert_eq!(speeds, vec![speed], "the tap writes no speed");
    assert!(!fault_latched(&sim), "no fault");
}

// [test->sys~ops_002~1]
#[test]
fn a_host_method_change_holds_its_target_until_the_dial_moves() {
    // The controls run the board's default method with the dial a sixth of the
    // way up; the host then stops it and starts the velocity method at 2 rad/s.
    // The dial, still where it stood, writes nothing over the host's target; a
    // dial turn then writes the dial's.
    const DIAL_DEG: f64 = 30.0;
    const HOST_SPEED: f32 = 2.0;
    let mut sim = connected_board();
    tap_button(&mut sim);
    sim.run_for_ms(align_dwell_ms(&sim) + 100);
    let mut deg = 0.0;
    while deg < DIAL_DEG {
        deg += 5.0;
        sim.write(&vid(DIAL, "angle[deg]"), deg).expect("turn the dial");
        sim.run_for_ms(20);
    }
    sim.run_for_ms(200);
    assert!(bridge_enabled(&sim), "the controls run the default method");

    let (reply, _) = exchange(&mut sim, 1, command(MODE_OFF, vec![]));
    assert!(accepted(&reply), "the host stops the drive");
    sim.run_for_ms(20);
    assert!(!bridge_enabled(&sim), "the drive is off");
    let (reply, _) = exchange(&mut sim, 2, command(MODE_FOC_VELOCITY, vec![speed_setpoint(0, HOST_SPEED, 0.0)]));
    assert!(accepted(&reply), "the host starts the velocity method");
    let held = speed_requests_over(&mut sim, 500);
    println!("command_authority: speed requests after the host's method change {held:?}");
    assert_eq!(held.last().copied(), Some(f64::from(HOST_SPEED)), "the host's target stands: {held:?}");
    assert!(
        held.iter().all(|&s| (s == f64::from(HOST_SPEED)) || (s == held[0])),
        "no dial write while the dial stands: {held:?}"
    );

    sim.write(&vid(DIAL, "angle[deg]"), DIAL_DEG + 20.0).expect("turn the dial");
    let moved = speed_requests_over(&mut sim, 100);
    let dial_speed = ((DIAL_DEG + 20.0) / DIAL_FULL_SCALE_DEG) * f64_at(&sim, &format!("{MC_CONFIG}.maxVelocity_radPerSec"));
    println!("command_authority: speed requests after the dial turn {moved:?}, the dial's {dial_speed:.3} rad/s");
    assert!((moved.last().unwrap() - dial_speed).abs() < 0.05, "a dial turn writes the dial's target: {moved:?}");
    assert!(!fault_latched(&sim), "no fault");
}
