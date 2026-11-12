//! The host command plane end to end on the board world: a Command's timed
//! setpoints reach the shared targets on the device's own 1 ms clock, a stop
//! setpoint disables the drive at its time and ends the sequence, a sequence
//! plays on through a host disconnect, rejected requests come back as verdicts
//! with their causes, and GetMotorInfo reports the configured ratings.

use pcs_bldc_sil::board::{align_dwell_ms, board_with, GATE_BRINGUP_MS};
use pcs_bldc_sil::wire::frame;
use pcs_bldc_sil::{cid, MotorParams, Sil};

mod common;
use common::proto::{
    self, board::reply, command, get_motor_info, ping, position_setpoint as position_at, speed_setpoint as speed_at,
    stop_setpoint as stop_at, torque_setpoint as torque_at, Payload,
};
use common::{
    accepted, assert_status, bool_at, cause, enabled_world, exchange, f64_at, inject, inject_wire, mc, mc_config,
    select_method, tap_button, Host, Status, MC, MODE_FOC_POSITION, MODE_FOC_TORQUE, MODE_FOC_VELOCITY, MODE_OFF,
    MODE_SIX_STEP_TRAP,
};

const REQUESTED: &str = "app_motorControl_data.channels[0].velocitySetpointRequested_radPerSec";
const POSITION_SETPOINT: &str = "app_motorControl_data.channels[0].positionSetpoint";
const MODE_REQUESTED: &str = "app_motorControl_data.channels[0].modeRequested";
const BRIDGE_ENABLED: &str = "app_motorControl_data.channels[0].bridgeEnabled";
const SEQUENCE_RUNNING: &str = "app_sequencer_data.channels[0].running";
const OFF: Status = Status { name: "APP_MOTORCONTROL_MODE_OFF", ordinal: 0 };
const FOC_VELOCITY: Status = Status { name: "APP_MOTORCONTROL_MODE_FOC_VELOCITY", ordinal: 4 };

/// The board world with the host connected and the gate driver up.
fn connected_board() -> Sil {
    let mut sim = board_with(Sil::options(), 0.0).sim;
    sim.write(&cid("HW_USB_sim_data.connected"), true).expect("connect the host");
    sim.run_for_ms(GATE_BRINGUP_MS);
    sim
}

/// Run `ms` a millisecond at a time, recording every change of the firmware
/// float at `path` as (millisecond, value) on a clock from the call, and the
/// millisecond request `id` was answered on with its verdict.
fn record_changes(sim: &mut Sil, host: &mut Host, path: &str, id: u32, ms: u32) -> (Vec<(u32, f64)>, Option<(u32, bool)>) {
    let mut changes = Vec::new();
    let mut verdict = None;
    let mut last = f64_at(sim, path);
    for now in 1..=ms {
        sim.run_for_ms(1);
        if let Some(reply) = host.reply_to(sim, id) {
            verdict = Some((now, accepted(&reply)));
        }
        let value = f64_at(sim, path);
        if (value - last).abs() > 1e-6 {
            changes.push((now, value));
            last = value;
        }
    }
    (changes, verdict)
}

/// Assert setpoint `i` of `changes` is `value`, applied `offset_ms` after the
/// acceptance at `t_acc` within the spec's 2 ms.
fn assert_applied(changes: &[(u32, f64)], t_acc: u32, i: usize, value: f64, offset_ms: u32) {
    assert!((changes[i].1 - value).abs() < 1e-6, "setpoint {i} value: {changes:?}");
    assert!(
        changes[i].0.abs_diff(t_acc + offset_ms) <= 2,
        "setpoint {i} at {offset_ms} ms from acceptance ({t_acc}): {changes:?}"
    );
}

// [test->sys~ops_003~1]
// [test->fw~ops_001~1]
// [test->fw~conn_server_007~1]
#[test]
fn a_sequence_is_applied_on_the_device_clock() {
    let mut sim = connected_board();
    let request = command(MODE_FOC_VELOCITY, vec![speed_at(0, 2.0, 0.0), speed_at(250_000, 4.0, 0.0), speed_at(500_000, 6.0, 0.0)]);
    inject(&mut sim, &proto::envelope(1, request));

    // One clock from the inject: the verdict's millisecond is the acceptance, and
    // every change of the speed request is stamped on the same clock.
    let (changes, verdict) = record_changes(&mut sim, &mut Host::new(), REQUESTED, 1, 700);
    let (t_acc, ok) = verdict.expect("a verdict within 700 ms");
    assert!(ok, "the Command is accepted");
    assert_eq!(changes.len(), 3, "three setpoints applied: {changes:?}");
    assert_applied(&changes, t_acc, 0, 2.0, 0);
    assert_applied(&changes, t_acc, 1, 4.0, 250);
    assert_applied(&changes, t_acc, 2, 6.0, 500);
}

// [test->fw~ops_001~1]
// [test->fw~conn_server_007~1]
#[test]
fn a_stop_setpoint_disables_the_drive_at_its_time() {
    let mut sim = connected_board();
    let request = command(MODE_FOC_VELOCITY, vec![speed_at(0, 2.0, 0.0), stop_at(300_000)]);
    let (reply, _) = exchange(&mut sim, 5, request);
    assert!(accepted(&reply), "a stop setpoint is accepted under a method");
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

// [test->fw~ops_001~1]
#[test]
fn a_stop_mid_sequence_drops_the_later_setpoints() {
    // The stop at 200 ms ends the sequence: the speed at 300 ms is never written,
    // and the board reports the sequence running, then not, on its own clock and
    // over the wire in Telemetry.
    let mut sim = connected_board();
    let request = command(MODE_FOC_VELOCITY, vec![speed_at(0, 2.0, 0.0), stop_at(200_000), speed_at(300_000, 5.0, 0.0)]);
    inject(&mut sim, &proto::envelope(11, request));
    let mut host = Host::new();
    let mut verdict = None;
    let mut running_reported = Vec::new();
    for _ in 0..500 {
        sim.run_for_ms(1);
        for env in host.poll(&mut sim) {
            match env.payload {
                Some(Payload::Telemetry(t)) => running_reported.push(t.sequence_running),
                Some(reply) if env.request_id == 11 => verdict = Some(accepted(&reply)),
                _ => {}
            }
        }
    }
    assert_eq!(verdict, Some(true), "the Command is accepted");
    assert_eq!(f64_at(&sim, REQUESTED), 2.0, "the setpoint after the stop is never written");
    assert_status(&sim, MODE_REQUESTED, &OFF, "the stop requested MODE_OFF");
    assert!(!bool_at(&sim, SEQUENCE_RUNNING), "the player holds no running sequence");
    assert!(running_reported.contains(&true), "Telemetry reported the sequence running: {running_reported:?}");
    assert_eq!(running_reported.last(), Some(&false), "Telemetry reports it ended: {running_reported:?}");
}

// [test->fw~ops_001~1]
// [test->sys~ops_003~1]
#[test]
fn a_sequence_plays_on_through_a_host_disconnect() {
    // The host detaches 50 ms after the acceptance: the setpoints at 250 and
    // 500 ms still land at their times on the device clock.
    let mut sim = connected_board();
    let request = command(MODE_FOC_VELOCITY, vec![speed_at(0, 2.0, 0.0), speed_at(250_000, 4.0, 0.0), speed_at(500_000, 6.0, 0.0)]);
    inject(&mut sim, &proto::envelope(12, request));
    let mut host = Host::new();
    let (mut changes, verdict) = record_changes(&mut sim, &mut host, REQUESTED, 12, 50);
    let (t_acc, ok) = verdict.expect("a verdict within 50 ms");
    assert!(ok, "the Command is accepted");

    sim.write(&cid("HW_USB_sim_data.connected"), false).expect("detach the host");
    let (later, _) = record_changes(&mut sim, &mut host, REQUESTED, 12, 650);
    changes.extend(later.into_iter().map(|(ms, v)| (ms + 50, v)));
    assert_eq!(changes.len(), 3, "every setpoint applied with the host gone: {changes:?}");
    assert_applied(&changes, t_acc, 0, 2.0, 0);
    assert_applied(&changes, t_acc, 1, 4.0, 250);
    assert_applied(&changes, t_acc, 2, 6.0, 500);
    assert!(sim.read_bool(&cid(BRIDGE_ENABLED)), "the drive runs on with the host gone");
}

// [test->fw~conn_server_007~1]
#[test]
fn a_setpoint_past_the_rating_is_rejected_and_the_running_sequence_plays_on() {
    let mut sim = connected_board();
    // A witness sequence under the velocity method from a disabled board; its
    // 300 ms setpoint is the proof the rejection left it running.
    let witness = command(MODE_FOC_VELOCITY, vec![speed_at(0, 1.0, 0.0), speed_at(300_000, 9.0, 0.0)]);
    let (reply, _) = exchange(&mut sim, 2, witness);
    assert!(accepted(&reply), "the witness is accepted");

    let max_velocity = mc_config(&sim, "maxVelocity_radPerSec");
    let (reply, _) = exchange(&mut sim, 3, command(MODE_FOC_VELOCITY, vec![speed_at(0, 100.0, 0.0)]));
    assert!(!accepted(&reply), "100 rad/s is past the {max_velocity} rad/s rating");
    assert_eq!(cause(&reply), "setpoint exceeds the motor rating");

    sim.run_for_ms(400);
    assert_eq!(f64_at(&sim, REQUESTED), 9.0, "the running sequence played on through the rejection");
    assert_status(&sim, MODE_REQUESTED, &FOC_VELOCITY, "the witness's method stands");
    assert!(sim.read_bool(&cid(BRIDGE_ENABLED)), "the witness's drive runs on");

    let (reply, _) = exchange(&mut sim, 4, command(MODE_OFF, vec![]));
    assert!(accepted(&reply), "MODE_OFF with no setpoints stops");
}

// [test->fw~conn_server_007~1]
#[test]
fn a_setpoint_kind_the_mode_does_not_take_is_rejected() {
    let mut sim = connected_board();
    for (id, request, what) in [
        (13, command(MODE_FOC_VELOCITY, vec![torque_at(0, 0.1, 0.0)]), "a torque setpoint under the velocity method"),
        (14, command(MODE_OFF, vec![speed_at(0, 1.0, 0.0)]), "a speed setpoint under MODE_OFF"),
    ] {
        let (reply, _) = exchange(&mut sim, id, request);
        assert!(!accepted(&reply), "{what} is rejected");
        assert_eq!(cause(&reply), "setpoint kind not for this mode", "{what}");
    }
    assert!(!sim.read_bool(&cid(BRIDGE_ENABLED)), "nothing enabled the drive");
    assert!(!bool_at(&sim, SEQUENCE_RUNNING), "nothing reached the player");
}

// [test->fw~conn_server_001~1]
#[test]
fn an_oversize_frame_is_answered_and_the_request_behind_it_too() {
    // A well-formed frame one byte past the 448-byte payload bound, then a ping:
    // the first is rejected with request_id 0, the ping answered after it.
    let mut sim = connected_board();
    let mut wire = frame(&[0x08u8; 449]);
    wire.extend(frame(&proto::envelope(21, ping())));
    inject_wire(&mut sim, &wire);
    let mut host = Host::new();
    let mut replies = Vec::new();
    for _ in 0..20 {
        sim.run_for_ms(1);
        replies.extend(host.poll(&mut sim).into_iter().filter(|env| !matches!(env.payload, Some(Payload::Telemetry(_)))));
    }
    assert_eq!(replies.len(), 2, "one reply per frame: {replies:?}");
    let oversize = replies[0].payload.as_ref().expect("a payload");
    assert_eq!(replies[0].request_id, 0, "an unread frame is answered with request_id 0");
    assert!(!accepted(oversize), "the oversize frame is rejected");
    assert_eq!(cause(oversize), "frame too large");
    assert_eq!(replies[1].request_id, 21, "the ping behind it is answered");
    assert!(accepted(replies[1].payload.as_ref().expect("a payload")), "the ping is accepted");
}

// [test->fw~mc_027~1]
#[test]
fn a_rate_limited_speed_setpoint_ramps_the_target_at_its_rate() {
    // 10 rad/s at 20 rad/s^2 over the wire onto the enabled velocity method at rest:
    // the target in effect advances 0.02 rad/s a cycle and stands at 10 rad/s
    // 500 ms after the write.
    const RATE: f64 = 20.0;
    let mut sim = enabled_world(MODE_FOC_VELOCITY, MotorParams::default());
    let start = mc(&sim, "velocitySetpoint_radPerSec");
    let (reply, _) = exchange(&mut sim, 15, command(MODE_FOC_VELOCITY, vec![speed_at(0, 10.0, RATE as f32)]));
    assert!(accepted(&reply), "the rate-limited setpoint is accepted");

    let mut targets = vec![mc(&sim, "velocitySetpoint_radPerSec")];
    for _ in 0..600 {
        sim.run_for_ms(1);
        targets.push(mc(&sim, "velocitySetpoint_radPerSec"));
    }
    let ramping: Vec<f64> = targets.windows(2).map(|w| w[1] - w[0]).filter(|d| d.abs() > 1e-6).collect();
    let (fastest, slowest) = ramping.iter().fold((f64::MIN, f64::MAX), |(hi, lo), &d| (hi.max(d), lo.min(d)));
    let step = RATE * 1e-3;
    println!("command: ramp from {start:+.4} rad/s over {} cycles, per-cycle advance {slowest:.5}..{fastest:.5} rad/s", ramping.len());
    // The last cycle of the ramp lands on the target and may advance less.
    for d in &ramping[..ramping.len() - 1] {
        assert!((d - step).abs() <= 0.01 * step, "each cycle advances {step} rad/s within 1 %: {d:.6}");
    }
    let cycles = ramping.len() as f64;
    assert!((cycles - ((10.0 - start) / step)).abs() <= 2.0, "the ramp takes (10 - start) / {step} cycles: {cycles}");
    assert!((targets.last().unwrap() - 10.0).abs() < 1e-5, "the target stands at 10 rad/s");
}

// [test->fw~conn_server_007~1]
// [test->fw~ops_001~1]
#[test]
fn a_position_setpoint_is_accepted_and_reaches_the_position_setpoint() {
    let mut sim = connected_board();
    let request = command(MODE_FOC_POSITION, vec![position_at(100_000, 6.0, 10.0, 50.0, 2.0)]);
    let (reply, _) = exchange(&mut sim, 6, request);
    assert!(accepted(&reply), "a position setpoint within its bounds is accepted under the position method");
    let field_at = |sim: &Sil, name: &str| f64_at(sim, &format!("{POSITION_SETPOINT}.{name}"));
    sim.run_for_ms(80);
    assert_eq!(field_at(&sim, "position_rad"), 0.0, "not yet written at 80 ms");
    sim.run_for_ms(30);
    assert!((field_at(&sim, "position_rad") - 6.0).abs() < 1e-6, "the position written at its 100 ms");
    assert!((field_at(&sim, "velocityLimit_radPerSec") - 10.0).abs() < 1e-6, "with its velocity limit");
    assert!((field_at(&sim, "accelLimit_radPerSec2") - 50.0).abs() < 1e-6, "its acceleration limit");
    assert!((field_at(&sim, "velocityAtPoint_radPerSec") - 2.0).abs() < 1e-6, "and its velocity at the point");
}

// [test->fw~conn_server_007~1]
#[test]
fn a_position_setpoint_past_its_bounds_is_rejected() {
    let mut sim = connected_board();
    let max_position = mc_config(&sim, "foc.maxPosition_rad");
    let max_velocity = mc_config(&sim, "maxVelocity_radPerSec") as f32;
    let rejected = |sim: &mut Sil, id: u32, setpoint, what: &str| {
        let (reply, _) = exchange(sim, id, command(MODE_FOC_POSITION, vec![setpoint]));
        assert!(!accepted(&reply), "{what} is rejected");
        assert!(!cause(&reply).is_empty(), "the verdict names a cause for {what}");
    };
    rejected(&mut sim, 7, position_at(0, (max_position + 1.0) as f32, 10.0, 50.0, 0.0), "a position past the configured bound");
    rejected(&mut sim, 8, position_at(0, 1.0, max_velocity + 1.0, 50.0, 0.0), "a velocity limit past the rating");
    rejected(&mut sim, 9, position_at(0, 1.0, 10.0, 50.0, 12.0), "a velocity at the point past its 10 rad/s limit");
    rejected(&mut sim, 10, position_at(0, 1.0, 0.0, 50.0, max_velocity + 1.0), "a velocity at the point past the rating at a zero limit");
    assert!(!sim.read_bool(&cid(BRIDGE_ENABLED)), "nothing enabled the drive");
}

// [test->fw~conn_server_008~1]
// [test->sys~obs_011~1]
#[test]
fn get_motor_info_reports_the_board_ratings() {
    // The wire carries the channel config's own values, read out of the image.
    let mut sim = connected_board();
    let (reply, _) = exchange(&mut sim, 4, get_motor_info());
    let Payload::BoardReply(proto::board::Reply { result: Some(reply::Result::MotorInfo(info)) }) = reply else {
        panic!("a MotorInfo result, not a Response: {reply:?}");
    };
    let kt = mc_config(&sim, "foc.torqueConstant_NmPerA") as f32;
    let i_max = mc_config(&sim, "foc.maxCurrent_A") as f32;
    assert_eq!(info.kt_nm_per_a, kt, "K_t");
    assert_eq!(info.i_max_a, i_max, "I_max");
    assert_eq!(info.max_torque_nm, kt * i_max, "max torque is K_t * I_max");
    assert_eq!(info.max_velocity_radps, mc_config(&sim, "maxVelocity_radPerSec") as f32, "max velocity");
    assert_eq!(info.max_position_rad, mc_config(&sim, "foc.maxPosition_rad") as f32, "max position");
}

const MODE_SELECTED: &str = "app_motorControl_data.channels[0].modeSelected";
const SIX_STEP: Status = Status { name: "APP_MOTORCONTROL_MODE_SIX_STEP_TRAP", ordinal: 1 };

// [test->fw~mc_033~1]
// [test->fw~conn_server_007~1]
#[test]
fn a_method_change_while_driving_is_refused_and_off_lets_it_through() {
    // Six-step drives off the button; the velocity method named over the host is
    // refused and six-step keeps commutating. MODE_OFF disables, and the velocity
    // method's request then selects it.
    let mut sim = connected_board();
    select_method(&mut sim, MODE_SIX_STEP_TRAP);
    tap_button(&mut sim);
    sim.run_for_ms(align_dwell_ms(&sim) + 100);
    assert!(sim.read_bool(&cid(BRIDGE_ENABLED)), "six-step drives");

    let (reply, _) = exchange(&mut sim, 30, command(MODE_FOC_VELOCITY, vec![speed_at(0, 2.0, 0.0)]));
    assert!(!accepted(&reply), "another method is refused while six-step drives");
    assert_eq!(cause(&reply), "bridge enabled");
    sim.run_for_ms(50);
    assert_status(&sim, MODE_SELECTED, &SIX_STEP, "six-step stays selected");
    assert!(sim.read_bool(&cid(BRIDGE_ENABLED)), "and commutating");

    let (reply, _) = exchange(&mut sim, 31, command(MODE_OFF, vec![]));
    assert!(accepted(&reply), "OFF is honoured");
    sim.run_for_ms(20);
    assert!(!sim.read_bool(&cid(BRIDGE_ENABLED)), "OFF disables the bridge");
    assert_status(&sim, MODE_SELECTED, &SIX_STEP, "the selection is kept through OFF");
    let (reply, _) = exchange(&mut sim, 32, command(MODE_FOC_VELOCITY, vec![speed_at(0, 0.0, 0.0)]));
    assert!(accepted(&reply), "the velocity method's request is taken once dark");
    sim.run_for_ms(20);
    assert_status(&sim, MODE_SELECTED, &FOC_VELOCITY, "and selects it");
}

// [test->fw~mc_026~1]
#[test]
fn a_torque_written_while_dark_is_the_target_after_the_enable() {
    // The Command's torque lands on the first pass, while the bridge still
    // precharges and aligns: it is the target in effect once the method drives.
    let mut sim = connected_board();
    let (reply, _) = exchange(&mut sim, 33, command(MODE_FOC_TORQUE, vec![torque_at(0, 0.1, 0.0)]));
    assert!(accepted(&reply), "the torque method is requested with its target");
    assert!(!sim.read_bool(&cid(BRIDGE_ENABLED)), "the bridge is still dark when the target lands");
    let mut first_driving = None;
    for _ in 0..(align_dwell_ms(&sim) + 200) {
        sim.run_for_ms(1);
        if first_driving.is_none() && sim.read_bool(&cid(BRIDGE_ENABLED)) && bool_at(&sim, &format!("{MC}.isAligned")) {
            first_driving = Some(mc(&sim, "torqueSetpoint_Nm"));
        }
    }
    println!("command: torque written dark, in effect {first_driving:?} Nm on the first aligned driving pass");
    let target = first_driving.expect("the method drives after its alignment");
    assert!((target - 0.1).abs() < 1e-6, "the torque written while dark is the target: {target}");
}
