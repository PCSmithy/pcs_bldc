//! Estimator order characterisation (tools/trace_analysis/estimator_order): the
//! plan's scenarios, each run as one Command over the host plane on the board
//! world and recorded every PWM cycle to CSV for the notebook. A capture, not
//! a check, so ignored by default:
//!
//!   cargo nextest run -p pcs_bldc_sil --release --run-ignored ignored-only -E 'binary(estimator_order)'
//!
//! `PCS_EST_DESIGN` labels the firmware under test (else from the build-dir
//! suffix `PCS_SIL_DLL` names, `native-fw-est<design>`, else C, the branch
//! head); `PCS_EST_OUT` overrides the output directory. For a sweep,
//! `PCS_EST_VARIANTS=nominal` runs the 1x plant alone and `PCS_EST_PLANT_TC_NM`
//! / `PCS_EST_PLANT_B` replace the plant's Coulomb and viscous friction.

mod common;

use common::proto::{
    self, command, field_varint, parse_envelope, parse_fields, speed_setpoint, stop_setpoint, torque_setpoint,
    F_BOARD_REQUEST, F_RESPONSE,
};
use common::{drain_tx, f64_at, inject, plant, select_method, tap_button, GRID_US, MODE_FOC_TORQUE, MODE_FOC_VELOCITY};
use pcs_bldc_sil::board::{board_with_motor, fault_latched, ALIGN_DWELL_MS, GATE_BRINGUP_MS};
use pcs_bldc_sil::wire::Deframer;
use pcs_bldc_sil::{MotorParams, Sil, SilOptions};
use std::fmt::Write as _;
use std::path::PathBuf;

const EST: &str = "app_rotorEstimation_data.channels[0]";
const MC: &str = "app_motorControl_data.channels[0]";

/// The sequence's duration: every run ends in the stop there, as the widget's do.
const DURATION_MS: u32 = 1000;
/// Recorded past the stop, so the coast-down after it is in the capture.
const RECORD_MS: u32 = 1300;
const CYCLES_PER_MS: u32 = (1000 / GRID_US) as u32;

/// One point: time from the sequence's start, the target in the mode's unit,
/// its rate limit.
type Point = (u32, f32, f32);

struct Scenario {
    name: &'static str,
    mode: u64,
    points: &'static [Point],
}

/// The plant variants every design runs at: the inertia at 0.5x, 1x and 2x the
/// estimator's configured J (read out of the firmware's channel config), and
/// the Coulomb friction at 4x the plant model's as the unmodelled load. Named
/// `_J<factor>[_Tc<factor>]`.
const VARIANTS: [(f64, f64); 4] = [(0.5, 1.0), (1.0, 1.0), (2.0, 1.0), (1.0, 4.0)];

/// The firmware columns, named by DWARF path as the app's export names them, so
/// one notebook reads SIL and bench captures alike.
fn firmware_columns() -> Vec<String> {
    vec![
        format!("{EST}.x_hat.e[0][0]"),
        format!("{EST}.x_hat.e[1][0]"),
        format!("{EST}.x_hat.e[2][0]"),
        format!("{MC}.iQ_a"),
        format!("{EST}.angleMeasured_rad"),
        format!("{EST}.angleError_rad"),
        format!("{EST}.u"),
        format!("{MC}.torqueSetpoint_Nm"),
        format!("{MC}.velocitySetpoint_radPerSec"),
    ]
}

fn design() -> String {
    std::env::var("PCS_EST_DESIGN").unwrap_or_else(|_| {
        let dll = std::env::var("PCS_SIL_DLL").unwrap_or_default();
        dll.split_once("native-fw-est")
            .and_then(|(_, rest)| rest.split(['/', '\\']).next())
            .filter(|d| !d.is_empty())
            .map_or_else(|| "C".to_string(), str::to_string)
    })
}

fn env_f64(name: &str) -> Option<f64> {
    std::env::var(name).ok().and_then(|s| s.parse().ok())
}

/// The plant model's base parameters, with the sweep overrides applied.
fn plant_base() -> MotorParams {
    let mut base = MotorParams::default();
    if let Some(tc) = env_f64("PCS_EST_PLANT_TC_NM") {
        base.t_c_nm = tc;
    }
    if let Some(b) = env_f64("PCS_EST_PLANT_B") {
        base.b_nm_per_rad_s = b;
    }
    base
}

fn variants() -> Vec<(f64, f64)> {
    if std::env::var("PCS_EST_VARIANTS").is_ok_and(|v| v == "nominal") { vec![(1.0, 1.0)] } else { VARIANTS.to_vec() }
}

fn out_dir() -> PathBuf {
    std::env::var_os("PCS_EST_OUT").map(PathBuf::from).unwrap_or_else(|| {
        PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../../tools/trace_analysis/estimator_order/sil")
    })
}

/// The estimator's configured inertia, read out of the firmware under test.
fn configured_inertia() -> f64 {
    let sim = board_with_motor(SilOptions::default().grid_us(GRID_US), 0.0, MotorParams::default()).sim;
    f64_at(&sim, "app_rotorEstimation_channelConfig[0].inertia_kgm2")
}

/// The board world on a plant variant, the scenario's method selected, enabled
/// and aligned, the host connected.
fn enabled_world(mode: u64, inertia_factor: f64, coulomb_factor: f64) -> Sil {
    let base = plant_base();
    let motor = MotorParams { j_kg_m2: configured_inertia() * inertia_factor, t_c_nm: base.t_c_nm * coulomb_factor, ..base };
    let mut sim = board_with_motor(SilOptions::default().grid_us(GRID_US), 0.0, motor).sim;
    sim.run_for_ms(GATE_BRINGUP_MS);
    select_method(&mut sim, mode);
    tap_button(&mut sim);
    sim.run_for_ms(ALIGN_DWELL_MS + 100);
    assert!(sim.read_bool(&pcs_bldc_sil::cid(&format!("{MC}.isAligned"))), "aligned before the run");
    assert!(!fault_latched(&sim), "no fault through the enable");
    sim
}

/// The sequence as the widget would send it: the points, then the stop.
fn sequence(scenario: &Scenario) -> Vec<u8> {
    let mut setpoints: Vec<Vec<u8>> = scenario
        .points
        .iter()
        .map(|&(t_ms, value, rate)| match scenario.mode {
            MODE_FOC_TORQUE => torque_setpoint(t_ms * 1000, value, rate),
            _ => speed_setpoint(t_ms * 1000, value, rate),
        })
        .collect();
    setpoints.push(stop_setpoint(DURATION_MS * 1000));
    command(scenario.mode, &setpoints)
}

/// The board's verdict on request `id`, read out of the transmit capture.
fn verdict(deframer: &mut Deframer, sim: &mut Sil, id: u64) -> Option<bool> {
    let capture = drain_tx(sim);
    for payload in deframer.push(&capture) {
        if let Some((rid, field, bytes)) = parse_envelope(&payload) {
            if (rid == id) && (field == F_RESPONSE) {
                return Some(field_varint(&parse_fields(&bytes), 1) == 1);
            }
        }
    }
    None
}

/// Run one scenario on one plant variant and write its capture; the time axis
/// starts at the Command's injection.
fn capture(scenario: &Scenario, inertia_factor: f64, coulomb_factor: f64) {
    let name = scenario.name;
    let mut sim = enabled_world(scenario.mode, inertia_factor, coulomb_factor);
    inject(&mut sim, &proto::envelope(1, F_BOARD_REQUEST, &sequence(scenario)));

    let columns = firmware_columns();
    let mut text = String::new();
    writeln!(text, "time_ms,{},plant.angle_rad,plant.velocity_radps", columns.join(",")).unwrap();
    let mut deframer = Deframer::new();
    let mut accepted = None;
    for cycle in 0..(RECORD_MS * CYCLES_PER_MS) {
        sim.run_for_us(GRID_US);
        let t_ms = f64::from(cycle + 1) / f64::from(CYCLES_PER_MS);
        write!(text, "{t_ms:.2}").unwrap();
        for path in &columns {
            write!(text, ",{:.7}", f64_at(&sim, path)).unwrap();
        }
        writeln!(text, ",{:.7},{:.7}", plant(&sim, "angle"), plant(&sim, "velocity")).unwrap();
        if accepted.is_none() && ((cycle + 1) % CYCLES_PER_MS == 0) {
            accepted = verdict(&mut deframer, &mut sim, 1);
        }
    }
    assert_eq!(accepted, Some(true), "{name}: the Command is accepted");
    assert!(!fault_latched(&sim), "{name}: no fault through the run");

    let dir = out_dir();
    std::fs::create_dir_all(&dir).expect("the capture directory");
    let tc = if coulomb_factor == 1.0 { String::new() } else { format!("_Tc{coulomb_factor}") };
    let file = dir.join(format!("{name}_{}_J{inertia_factor}{tc}.csv", design()));
    std::fs::write(&file, text).expect("write the capture");
    println!("estimator_order: {} (plant at {:+.1} rad/s at the end)", file.display(), plant(&sim, "velocity"));
}

/// The captures' provenance beside them: the firmware's commit and the DLL.
fn write_provenance() {
    let describe = std::process::Command::new("git")
        .args(["describe", "--always", "--dirty"])
        .current_dir(env!("CARGO_MANIFEST_DIR"))
        .output()
        .map(|o| String::from_utf8_lossy(&o.stdout).trim().to_string())
        .unwrap_or_else(|_| "unknown".to_string());
    let dll = std::env::var("PCS_SIL_DLL").unwrap_or_else(|_| "build-tree default".to_string());
    let dir = out_dir();
    std::fs::create_dir_all(&dir).expect("the capture directory");
    std::fs::write(dir.join(format!("provenance_{}.txt", design())), format!("design {}\ncommit {describe}\ndll {dll}\n", design()))
        .expect("write the provenance");
}

fn run(scenario: &Scenario) {
    write_provenance();
    for (inertia_factor, coulomb_factor) in variants() {
        capture(scenario, inertia_factor, coulomb_factor);
    }
}

/// A step of acceleration, a slow decay, a coast to rest.
#[test]
#[ignore]
fn s1_step() {
    run(&Scenario { name: "S1_step", mode: MODE_FOC_TORQUE, points: &[(100, 0.08, 0.0), (160, 0.01, 0.0), (700, 0.0, 0.0)] });
}

/// A torque triangle under the voltage ceiling: ramped acceleration, the input
/// term's tracking.
#[test]
#[ignore]
fn s2_ramp() {
    run(&Scenario { name: "S2_ramp", mode: MODE_FOC_TORQUE, points: &[(100, 0.04, 0.25), (260, 0.01, 0.25), (800, 0.0, 0.25)] });
}

/// Reversals through zero speed, where the friction model is wrong, under the
/// voltage ceiling.
#[test]
#[ignore]
fn s3_reversal() {
    run(&Scenario {
        name: "S3_reversal",
        mode: MODE_FOC_TORQUE,
        points: &[(100, 0.03, 0.0), (300, -0.03, 0.0), (600, 0.03, 0.0), (900, 0.0, 0.0)],
    });
}

/// The estimator's consumer: a velocity step under the velocity method.
#[test]
#[ignore]
fn s4_velocity_step() {
    run(&Scenario { name: "S4_velstep", mode: MODE_FOC_VELOCITY, points: &[(100, 10.0, 0.0), (700, 0.0, 0.0)] });
}

/// A crawl under the velocity method: 0.5 rad/s held through the stop, the
/// regime of the friction model's ramp band and stiction.
#[test]
#[ignore]
fn s5_crawl() {
    run(&Scenario { name: "S5_crawl", mode: MODE_FOC_VELOCITY, points: &[(100, 0.5, 0.0)] });
}
