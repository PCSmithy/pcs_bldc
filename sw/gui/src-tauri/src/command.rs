//! The board command plane (app~conn_004): a `Command`, a mode with its timed
//! setpoint list, and `GetMotorInfo`, sent over the session and resolved to
//! the UI as the board's verdict or result.

use std::time::Duration;

use pcs_proto::board;
use pcs_proto::shared::envelope::Payload;
use prost::Message;
use tauri::{AppHandle, Manager};

use crate::protocol::{payload_kind, Client};
use crate::session::{session_handles, SessionState};

/// One setpoint as the UI states it: a time from the start, one target (speed,
/// torque, position, or stop), and a rate limit (zero steps); a position's is
/// its velocity limit, beside its acceleration limit and velocity at the point.
#[derive(Clone, Debug, Default, serde::Deserialize)]
pub struct SetpointSpec {
    pub t_us: u32,
    #[serde(default)]
    pub velocity_radps: Option<f32>,
    #[serde(default)]
    pub torque_nm: Option<f32>,
    #[serde(default)]
    pub position_rad: Option<f32>,
    #[serde(default)]
    pub stop: Option<StopSpec>,
    #[serde(default)]
    pub rate_limit: f32,
    #[serde(default)]
    pub accel_limit_radps2: f32,
    #[serde(default)]
    pub velocity_at_point_radps: f32,
}

/// The stop target, `{}` as the UI states it.
#[derive(Clone, Debug, Default, serde::Deserialize)]
pub struct StopSpec {}

/// A mode by its `board.Mode` name, as telemetry reports it, and its list.
#[derive(Clone, Debug, serde::Deserialize)]
pub struct CommandSpec {
    pub mode: String,
    #[serde(default)]
    pub setpoints: Vec<SetpointSpec>,
}

#[derive(Clone, Debug, PartialEq, serde::Serialize)]
pub struct Verdict {
    pub accepted: bool,
    pub cause: String,
}

#[derive(Clone, Debug, PartialEq, serde::Serialize)]
pub struct MotorInfo {
    pub kt_nm_per_a: f32,
    pub i_max_a: f32,
    pub max_torque_nm: f32,
    pub max_velocity_radps: f32,
    pub max_position_rad: f32,
}

/// A Command landing before the previous one's start pass is rejected with
/// this cause (fw~conn_server_007, app_server_config.c); the host resends.
const BUSY_CAUSE: &str = "sequence player busy";
const BUSY_SENDS: u32 = 3;
const BUSY_RETRY_DELAY: Duration = Duration::from_millis(2);

/// The board's largest frame payload (IO_COBSFRAME_MAX_PAYLOAD); it answers a
/// larger frame `frame too large`, so the core refuses one unsent.
pub const MAX_FRAME_PAYLOAD: usize = 448;

// [impl->app~conn_004~1]
pub fn build_command(spec: &CommandSpec) -> Result<board::Command, String> {
    let mode = board::Mode::from_str_name(&spec.mode)
        .ok_or_else(|| format!("unknown mode {}", spec.mode))?;
    let mut setpoints = Vec::with_capacity(spec.setpoints.len());
    for (i, sp) in spec.setpoints.iter().enumerate() {
        let target = match (sp.velocity_radps, sp.torque_nm, sp.position_rad, sp.stop.is_some()) {
            (Some(v), None, None, false) => board::setpoint::Target::VelocityRadps(v),
            (None, Some(t), None, false) => board::setpoint::Target::TorqueNm(t),
            (None, None, Some(p), false) => board::setpoint::Target::PositionRad(p),
            (None, None, None, true) => board::setpoint::Target::Stop(board::Stop {}),
            _ => return Err(format!("setpoint {i}: exactly one target")),
        };
        setpoints.push(board::Setpoint {
            t_us: sp.t_us,
            rate_limit: sp.rate_limit,
            accel_limit_radps2: sp.accel_limit_radps2,
            velocity_at_point_radps: sp.velocity_at_point_radps,
            target: Some(target),
        });
    }
    let command = board::Command {
        mode: mode as i32,
        setpoints,
    };
    // Sized at the widest request id, so the frame fits whatever id it gets.
    let envelope = pcs_proto::shared::Envelope {
        request_id: u32::MAX,
        payload: Some(board_request(board::request::Command::Run(command.clone()))),
    };
    let len = envelope.encoded_len();
    if len > MAX_FRAME_PAYLOAD {
        return Err(format!("frame too large: {len} B, the board takes {MAX_FRAME_PAYLOAD} B"));
    }
    Ok(command)
}

fn board_request(command: board::request::Command) -> Payload {
    Payload::BoardRequest(board::Request {
        command: Some(command),
    })
}

fn verdict(reply: Payload) -> Result<Verdict, String> {
    match reply {
        Payload::Response(r) => Ok(Verdict {
            accepted: r.accepted,
            cause: r.cause,
        }),
        other => Err(format!("unexpected reply: {}", payload_kind(&other))),
    }
}

pub fn send_command_blocking(client: &Client, spec: &CommandSpec) -> Result<Verdict, String> {
    let command = build_command(spec)?;
    let mut sends = 0u32;
    loop {
        let run = board::request::Command::Run(command.clone());
        let result = verdict(client.request(board_request(run))?)?;
        sends += 1;
        if result.accepted || (result.cause != BUSY_CAUSE) || (sends >= BUSY_SENDS) {
            return Ok(result);
        }
        std::thread::sleep(BUSY_RETRY_DELAY);
    }
}

pub fn motor_info_blocking(client: &Client) -> Result<MotorInfo, String> {
    let query = board::request::Command::GetMotorInfo(board::GetMotorInfo::default());
    match client.request(board_request(query))? {
        Payload::BoardReply(board::Reply {
            result: Some(board::reply::Result::MotorInfo(m)),
        }) => Ok(MotorInfo {
            kt_nm_per_a: m.kt_nm_per_a,
            i_max_a: m.i_max_a,
            max_torque_nm: m.max_torque_nm,
            max_velocity_radps: m.max_velocity_radps,
            max_position_rad: m.max_position_rad,
        }),
        Payload::Response(r) => Err(format!("motor info rejected: {}", r.cause)),
        other => Err(format!("unexpected reply: {}", payload_kind(&other))),
    }
}

#[tauri::command]
pub async fn send_command(app: AppHandle, command: CommandSpec) -> Result<Verdict, String> {
    tauri::async_runtime::spawn_blocking(move || {
        let (client, _, _) = session_handles(&app.state::<SessionState>())?;
        send_command_blocking(&client, &command)
    })
    .await
    .map_err(|e| format!("send command: {e}"))?
}

#[tauri::command]
pub async fn get_motor_info(app: AppHandle) -> Result<MotorInfo, String> {
    tauri::async_runtime::spawn_blocking(move || {
        let (client, _, _) = session_handles(&app.state::<SessionState>())?;
        motor_info_blocking(&client)
    })
    .await
    .map_err(|e| format!("motor info: {e}"))?
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::testutil::{wait_for_requests, SharedBuf};
    use std::sync::Arc;

    fn reply_frame(request_id: u32, payload: Payload) -> Vec<u8> {
        let env = pcs_proto::shared::Envelope {
            request_id,
            payload: Some(payload),
        };
        pcs_wire::frame(&env.encode_to_vec())
    }

    fn response(accepted: bool, cause: &str) -> Payload {
        Payload::Response(pcs_proto::shared::Response {
            accepted,
            cause: cause.into(),
        })
    }

    /// The board requests written so far, decoded in write order.
    fn written_requests(buf: &SharedBuf) -> Vec<board::Request> {
        let wire = buf.0.lock().unwrap().clone();
        pcs_wire::parse_frames(&wire)
            .iter()
            .map(|f| match pcs_proto::shared::Envelope::decode(f.as_slice()).unwrap().payload {
                Some(Payload::BoardRequest(r)) => r,
                other => panic!("not a board request: {other:?}"),
            })
            .collect()
    }

    fn two_setpoints() -> CommandSpec {
        CommandSpec {
            mode: "MODE_FOC_VELOCITY".into(),
            setpoints: vec![
                SetpointSpec { t_us: 0, velocity_radps: Some(2.0), rate_limit: 0.0, ..Default::default() },
                SetpointSpec { t_us: 250_000, velocity_radps: Some(4.0), rate_limit: 20.0, ..Default::default() },
            ],
        }
    }

    // [test->app~conn_004~1]
    #[test]
    fn a_command_encodes_the_mode_and_its_setpoints_in_order() {
        let buf = SharedBuf::default();
        let client = Arc::new(Client::new(Box::new(buf.clone())));
        let mut pump = client.pump(Box::new(|_| {}));
        let worker = {
            let c = client.clone();
            std::thread::spawn(move || send_command_blocking(&c, &two_setpoints()))
        };
        let ids = wait_for_requests(&buf, 1);

        let requests = written_requests(&buf);
        let Some(board::request::Command::Run(cmd)) = &requests[0].command else {
            panic!("not a Command: {:?}", requests[0]);
        };
        assert_eq!(cmd.mode, board::Mode::FocVelocity as i32);
        assert_eq!(cmd.setpoints.len(), 2);
        assert_eq!(cmd.setpoints[0].t_us, 0);
        assert_eq!(cmd.setpoints[0].target, Some(board::setpoint::Target::VelocityRadps(2.0)));
        assert_eq!(cmd.setpoints[0].rate_limit, 0.0);
        assert_eq!(cmd.setpoints[1].t_us, 250_000);
        assert_eq!(cmd.setpoints[1].target, Some(board::setpoint::Target::VelocityRadps(4.0)));
        assert_eq!(cmd.setpoints[1].rate_limit, 20.0);

        pump.push(&reply_frame(ids[0], response(true, "")));
        assert_eq!(worker.join().unwrap(), Ok(Verdict { accepted: true, cause: String::new() }));
    }

    // [test->app~conn_004~1]
    #[test]
    fn a_rejected_command_resolves_with_the_boards_cause() {
        let buf = SharedBuf::default();
        let client = Arc::new(Client::new(Box::new(buf.clone())));
        let mut pump = client.pump(Box::new(|_| {}));
        let worker = {
            let c = client.clone();
            std::thread::spawn(move || send_command_blocking(&c, &two_setpoints()))
        };
        let ids = wait_for_requests(&buf, 1);
        pump.push(&reply_frame(ids[0], response(false, "setpoint exceeds the motor rating")));
        assert_eq!(
            worker.join().unwrap(),
            Ok(Verdict { accepted: false, cause: "setpoint exceeds the motor rating".into() })
        );
    }

    // [test->app~conn_004~1]
    #[test]
    fn a_busy_rejection_is_resent_and_the_second_verdict_stands() {
        let buf = SharedBuf::default();
        let client = Arc::new(Client::new(Box::new(buf.clone())));
        let mut pump = client.pump(Box::new(|_| {}));
        let worker = {
            let c = client.clone();
            std::thread::spawn(move || send_command_blocking(&c, &two_setpoints()))
        };
        let first = wait_for_requests(&buf, 1);
        pump.push(&reply_frame(first[0], response(false, "sequence player busy")));
        let both = wait_for_requests(&buf, 2);
        pump.push(&reply_frame(both[1], response(true, "")));
        assert_eq!(worker.join().unwrap(), Ok(Verdict { accepted: true, cause: String::new() }));
        assert_eq!(written_requests(&buf).len(), 2);
    }

    // [test->app~conn_004~1]
    #[test]
    fn three_busy_rejections_resolve_rejected_after_three_sends() {
        let buf = SharedBuf::default();
        let client = Arc::new(Client::new(Box::new(buf.clone())));
        let mut pump = client.pump(Box::new(|_| {}));
        let worker = {
            let c = client.clone();
            std::thread::spawn(move || send_command_blocking(&c, &two_setpoints()))
        };
        for n in 1..=3 {
            let ids = wait_for_requests(&buf, n);
            pump.push(&reply_frame(ids[n - 1], response(false, BUSY_CAUSE)));
        }
        assert_eq!(
            worker.join().unwrap(),
            Ok(Verdict { accepted: false, cause: BUSY_CAUSE.into() })
        );
        assert_eq!(written_requests(&buf).len(), 3, "no fourth send");
    }

    // [test->app~conn_004~1]
    #[test]
    fn get_motor_info_resolves_the_five_fields() {
        let buf = SharedBuf::default();
        let client = Arc::new(Client::new(Box::new(buf.clone())));
        let mut pump = client.pump(Box::new(|_| {}));
        let worker = {
            let c = client.clone();
            std::thread::spawn(move || motor_info_blocking(&c))
        };
        let ids = wait_for_requests(&buf, 1);
        let requests = written_requests(&buf);
        assert!(matches!(requests[0].command, Some(board::request::Command::GetMotorInfo(_))));
        pump.push(&reply_frame(
            ids[0],
            Payload::BoardReply(board::Reply {
                result: Some(board::reply::Result::MotorInfo(board::MotorInfo {
                    kt_nm_per_a: 0.825,
                    i_max_a: 1.0,
                    max_torque_nm: 0.825,
                    max_velocity_radps: 20.944,
                    max_position_rad: 62.83,
                })),
            }),
        ));
        assert_eq!(
            worker.join().unwrap(),
            Ok(MotorInfo {
                kt_nm_per_a: 0.825,
                i_max_a: 1.0,
                max_torque_nm: 0.825,
                max_velocity_radps: 20.944,
                max_position_rad: 62.83,
            })
        );
    }

    #[test]
    fn a_setpoint_needs_exactly_one_target() {
        let spec = CommandSpec {
            mode: "MODE_FOC_TORQUE".into(),
            setpoints: vec![SetpointSpec { t_us: 0, velocity_radps: Some(1.0), torque_nm: Some(0.1), ..Default::default() }],
        };
        assert!(build_command(&spec).is_err());
        let spec = CommandSpec {
            mode: "MODE_FOC_POSITION".into(),
            setpoints: vec![SetpointSpec { t_us: 0, position_rad: Some(1.0), stop: Some(StopSpec {}), ..Default::default() }],
        };
        assert!(build_command(&spec).is_err());
        let spec = CommandSpec { mode: "MODE_WARP".into(), setpoints: Vec::new() };
        assert!(build_command(&spec).is_err());
    }

    // [test->app~conn_004~1]
    #[test]
    fn a_stop_setpoint_encodes_the_stop_target() {
        let spec = CommandSpec {
            mode: "MODE_FOC_VELOCITY".into(),
            setpoints: vec![SetpointSpec { t_us: 1_000_000, stop: Some(StopSpec {}), ..Default::default() }],
        };
        let cmd = build_command(&spec).expect("a stop is one target");
        assert_eq!(cmd.setpoints[0].t_us, 1_000_000);
        assert_eq!(cmd.setpoints[0].target, Some(board::setpoint::Target::Stop(board::Stop {})));
    }

    // [test->app~conn_004~1]
    #[test]
    fn a_position_setpoint_encodes_its_limits_and_velocity_at_the_point() {
        let spec = CommandSpec {
            mode: "MODE_FOC_POSITION".into(),
            setpoints: vec![
                SetpointSpec { t_us: 0, position_rad: Some(0.0), ..Default::default() },
                SetpointSpec {
                    t_us: 200_000,
                    position_rad: Some(6.283),
                    rate_limit: 10.0,
                    accel_limit_radps2: 50.0,
                    velocity_at_point_radps: 5.0,
                    ..Default::default()
                },
            ],
        };
        let cmd = build_command(&spec).expect("a position is one target");
        assert_eq!(cmd.mode, board::Mode::FocPosition as i32);
        assert_eq!(cmd.setpoints[0].target, Some(board::setpoint::Target::PositionRad(0.0)));
        assert_eq!(cmd.setpoints[0].accel_limit_radps2, 0.0);
        assert_eq!(cmd.setpoints[0].velocity_at_point_radps, 0.0);
        let sp = &cmd.setpoints[1];
        assert_eq!(sp.t_us, 200_000);
        assert_eq!(sp.target, Some(board::setpoint::Target::PositionRad(6.283)));
        assert_eq!(sp.rate_limit, 10.0);
        assert_eq!(sp.accel_limit_radps2, 50.0);
        assert_eq!(sp.velocity_at_point_radps, 5.0);
    }

    // [test->app~conn_004~1]
    #[test]
    fn a_command_over_the_boards_frame_is_refused_unsent() {
        let point = |i: u32| SetpointSpec {
            t_us: 100_000 * (i + 1),
            position_rad: Some(1.5),
            rate_limit: 10.0,
            accel_limit_radps2: 50.0,
            velocity_at_point_radps: 5.0,
            ..Default::default()
        };
        let spec = CommandSpec { mode: "MODE_FOC_POSITION".into(), setpoints: (0..20).map(point).collect() };
        let err = build_command(&spec).expect_err("20 positions with their limits exceed 448 B");
        assert!(err.starts_with("frame too large: "), "{err}");

        let buf = SharedBuf::default();
        let client = Client::new(Box::new(buf.clone()));
        assert_eq!(send_command_blocking(&client, &spec), Err(err));
        assert!(buf.0.lock().unwrap().is_empty(), "nothing written");

        let fits = CommandSpec { mode: "MODE_FOC_POSITION".into(), setpoints: (0..10).map(point).collect() };
        assert!(build_command(&fits).is_ok());
    }

    // [test->app~conn_004~1]
    #[test]
    fn a_position_setpoint_deserialises_from_the_ui() {
        let spec: CommandSpec = serde_json::from_str(
            r#"{"mode":"MODE_FOC_POSITION","setpoints":[{"t_us":0,"position_rad":3.0,"rate_limit":0,
                "accel_limit_radps2":20,"velocity_at_point_radps":0},{"t_us":1000000,"stop":{}}]}"#,
        )
        .expect("the UI's position setpoint");
        let cmd = build_command(&spec).expect("encodes");
        assert_eq!(cmd.setpoints[0].target, Some(board::setpoint::Target::PositionRad(3.0)));
        assert_eq!(cmd.setpoints[0].accel_limit_radps2, 20.0);
        assert_eq!(cmd.setpoints[1].target, Some(board::setpoint::Target::Stop(board::Stop {})));
    }
}
