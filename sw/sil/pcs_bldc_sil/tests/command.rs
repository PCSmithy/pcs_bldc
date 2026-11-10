//! The host command plane end to end on the board world: a Command's timed
//! setpoints reach the shared speed target on the device's own 1 ms clock, a
//! stop setpoint disables the drive at its time, a setpoint past the rating
//! comes back as a verdict, and GetMotorInfo reports the board's ratings.

use pcs_bldc_sil::board::{board_with, GATE_BRINGUP_MS};
use pcs_bldc_sil::{cid, wire::Deframer, Sil};

mod common;
use common::proto::{
    self, command, field_bytes, field_varint, parse_envelope, parse_fields, put_len, speed_setpoint as speed_at,
    stop_setpoint as stop_at, F_BOARD_REPLY, F_BOARD_REQUEST, F_RESPONSE,
};
use common::{assert_status, drain_tx, f64_at, inject, Status, MODE_FOC_VELOCITY, MODE_OFF};

const REQUESTED: &str = "app_motorControl_data.channels[0].velocitySetpointRequested_radPerSec";
const MODE_REQUESTED: &str = "app_motorControl_data.channels[0].modeRequested";
const BRIDGE_ENABLED: &str = "app_motorControl_data.channels[0].bridgeEnabled";
const OFF: Status = Status { name: "APP_MOTORCONTROL_MODE_OFF", ordinal: 0 };
const FOC_VELOCITY: Status = Status { name: "APP_MOTORCONTROL_MODE_FOC_VELOCITY", ordinal: 4 };

/// `app_motorControl_channels.c`: K_t = 0.825 Nm/A, I_max = 1 A, 200 rpm.
const KT_NM_PER_A: f64 = 0.825;
const I_MAX_A: f64 = 1.0;
const MAX_VELOCITY_RAD_S: f64 = 200.0 * std::f64::consts::TAU / 60.0;

/// `board.Request { get_motor_info (3) }`.
fn get_motor_info() -> Vec<u8> {
    let mut req = Vec::new();
    put_len(3, &[], &mut req);
    req
}

/// The board world with the host connected and the gate driver up.
fn connected_board() -> Sil {
    let mut sim = board_with(Sil::options(), 0.0).sim;
    sim.write(&cid("HW_USB_sim_data.connected"), true).expect("connect the host");
    sim.run_for_ms(GATE_BRINGUP_MS);
    sim
}

/// Inject a board request and run until its reply: (payload field, bytes), and
/// the milliseconds it took.
fn exchange(sim: &mut Sil, id: u64, request: &[u8]) -> (u32, Vec<u8>, u32) {
    inject(sim, &proto::envelope(id, F_BOARD_REQUEST, request));
    let mut deframer = Deframer::new();
    for ms in 1..=50u32 {
        sim.run_for_ms(1);
        let capture = drain_tx(sim);
        for payload in deframer.push(&capture) {
            if let Some((rid, field, bytes)) = parse_envelope(&payload) {
                if rid == id {
                    return (field, bytes, ms);
                }
            }
        }
    }
    panic!("no reply to request {id} within 50 ms");
}

fn accepted(field: u32, bytes: &[u8]) -> bool {
    assert_eq!(field, F_RESPONSE, "a Response verdict");
    field_varint(&parse_fields(bytes), 1) == 1
}

// [test->sys~ops_003~1]
// [test->fw~conn_server_007~1]
#[test]
fn a_sequence_is_applied_on_the_device_clock() {
    let mut sim = connected_board();
    let request = command(
        MODE_FOC_VELOCITY,
        &[speed_at(0, 2.0, 0.0), speed_at(250_000, 4.0, 0.0), speed_at(500_000, 6.0, 0.0)],
    );
    inject(&mut sim, &proto::envelope(1, F_BOARD_REQUEST, &request));

    // One clock from the inject: the verdict's millisecond is the acceptance, and
    // every change of the speed request is stamped on the same clock.
    let mut deframer = Deframer::new();
    let mut accepted_ms: Option<u32> = None;
    let mut changes: Vec<(u32, f64)> = Vec::new();
    let mut last = f64_at(&sim, REQUESTED);
    for now in 1..=700u32 {
        sim.run_for_ms(1);
        for payload in deframer.push(&drain_tx(&mut sim)) {
            if let Some((1, field, bytes)) = parse_envelope(&payload) {
                assert!(accepted(field, &bytes), "the Command is accepted");
                accepted_ms = Some(now);
            }
        }
        let target = f64_at(&sim, REQUESTED);
        if (target - last).abs() > 1e-6 {
            changes.push((now, target));
            last = target;
        }
    }
    let t_acc = accepted_ms.expect("a verdict within 700 ms");
    assert_eq!(changes.len(), 3, "three setpoints applied: {changes:?}");
    let applied = |i: usize, value: f64, offset_ms: u32| {
        assert!((changes[i].1 - value).abs() < 1e-6, "setpoint {i} value: {changes:?}");
        assert!(changes[i].0.abs_diff(t_acc + offset_ms) <= 2, "setpoint {i} at {offset_ms} ms from acceptance ({t_acc}): {changes:?}");
    };
    applied(0, 2.0, 0);
    applied(1, 4.0, 250);
    applied(2, 6.0, 500);
}

// [test->fw~ops_001~1]
// [test->fw~conn_server_007~1]
#[test]
fn a_stop_setpoint_disables_the_drive_at_its_time() {
    let mut sim = connected_board();
    let request = command(MODE_FOC_VELOCITY, &[speed_at(0, 2.0, 0.0), stop_at(300_000)]);
    let (field, bytes, _) = exchange(&mut sim, 5, &request);
    assert!(accepted(field, &bytes), "a stop setpoint is accepted under a method");
    assert_status(&sim, MODE_REQUESTED, &FOC_VELOCITY, "the Command's mode is requested");

    sim.run_for_ms(280);
    assert_status(&sim, MODE_REQUESTED, &FOC_VELOCITY, "the mode stands until the stop's time");
    assert!(sim.read_bool(&cid(BRIDGE_ENABLED)), "the drive runs the sequence");
    assert_eq!(f64_at(&sim, REQUESTED), 2.0, "the speed setpoint was written");
    sim.run_for_ms(30);
    assert_status(&sim, MODE_REQUESTED, &OFF, "the stop at 300 ms requests MODE_OFF");
    assert!(!sim.read_bool(&cid(BRIDGE_ENABLED)), "the drive is off after the stop");
    assert_eq!(f64_at(&sim, REQUESTED), 2.0, "the stop writes no target");
}

// [test->fw~conn_server_007~1]
#[test]
fn a_setpoint_past_the_rating_is_rejected_and_the_running_sequence_plays_on() {
    let mut sim = connected_board();
    // A witness sequence under MODE_OFF, accepted in every state; its 300 ms
    // setpoint is the proof the rejection left it running.
    let witness = command(MODE_OFF, &[speed_at(0, 1.0, 0.0), speed_at(300_000, 9.0, 0.0)]);
    let (field, bytes, _) = exchange(&mut sim, 2, &witness);
    assert!(accepted(field, &bytes), "the witness is accepted");

    let (field, bytes, _) = exchange(&mut sim, 3, &command(MODE_FOC_VELOCITY, &[speed_at(0, 100.0, 0.0)]));
    assert!(!accepted(field, &bytes), "100 rad/s is past the 20.9 rad/s rating");
    let cause = String::from_utf8_lossy(&field_bytes(&parse_fields(&bytes), 2)).into_owned();
    assert!(!cause.is_empty(), "the verdict names a cause");

    sim.run_for_ms(400);
    assert_eq!(f64_at(&sim, REQUESTED), 9.0, "the running sequence played on through the rejection");
    assert!(!sim.read_bool(&cid(BRIDGE_ENABLED)), "the drive stayed off");

    let (field, bytes, _) = exchange(&mut sim, 4, &command(MODE_OFF, &[]));
    assert!(accepted(field, &bytes), "MODE_OFF with no setpoints stops");
}

// [test->fw~conn_server_008~1]
#[test]
fn get_motor_info_reports_the_board_ratings() {
    let mut sim = connected_board();
    let (field, bytes, _) = exchange(&mut sim, 4, &get_motor_info());
    assert_eq!(field, F_BOARD_REPLY, "a MotorInfo result, not a Response");
    let reply = parse_fields(&bytes);
    let info = parse_fields(&field_bytes(&reply, 1));
    let f = |field: u32| f64::from(f32::from_bits(field_varint(&info, field) as u32));
    let within = |got: f64, want: f64| (got - want).abs() <= 0.001 * want;
    assert!(within(f(1), KT_NM_PER_A), "K_t {}", f(1));
    assert!(within(f(2), I_MAX_A), "I_max {}", f(2));
    assert!(within(f(3), KT_NM_PER_A * I_MAX_A), "max torque {}", f(3));
    assert!(within(f(4), MAX_VELOCITY_RAD_S), "max velocity {}", f(4));
}
