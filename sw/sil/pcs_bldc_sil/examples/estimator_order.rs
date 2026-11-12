//! Estimator order characterisation (tools/trace_analysis/estimator_order): the
//! plan's scenarios, each run as one Command over the host plane on the board
//! world and recorded every PWM cycle to CSV for the notebook. From sw/sil:
//!
//!   PCS_SIL_DLL=<dll> PCS_EST_DESIGN=C cargo run --release -p pcs_bldc_sil --example estimator_order -- [scenario ...]
//!
//! Scenario names select a subset (S1_step S2_ramp S3_reversal S4_velstep
//! S5_crawl); none runs them all. `PCS_EST_DESIGN` labels the firmware under
//! test (else from the build-dir suffix `PCS_SIL_DLL` names, `native-fw-est<design>`,
//! else C); `PCS_EST_OUT` overrides the output directory. For a sweep,
//! `PCS_EST_VARIANTS=nominal` runs the 1x plant alone and `PCS_EST_PLANT_TC_NM`
//! / `PCS_EST_PLANT_B` replace the plant's Coulomb and viscous friction.

#[path = "../tests/common/mod.rs"]
mod common;

use common::proto::{self, command, speed_setpoint, stop_setpoint, torque_setpoint};
use common::{
    accepted, configured_inertia, enabled_world_from, f64_at, inject, plant, Host, Mode, EST, GRID_US, MC,
    MODE_FOC_TORQUE, MODE_FOC_VELOCITY,
};
use pcs_bldc_sil::board::fault_latched;
use pcs_bldc_sil::{CurrentSenseParams, MotorParams};
use std::fmt::Write as _;
use std::path::PathBuf;

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
    mode: Mode,
    points: &'static [Point],
}

const SCENARIOS: [Scenario; 5] = [
    // A step of acceleration, a slow decay, a coast to rest.
    Scenario { name: "S1_step", mode: MODE_FOC_TORQUE, points: &[(100, 0.08, 0.0), (160, 0.01, 0.0), (700, 0.0, 0.0)] },
    // A torque triangle under the voltage ceiling: ramped acceleration, the
    // input term's tracking.
    Scenario { name: "S2_ramp", mode: MODE_FOC_TORQUE, points: &[(100, 0.04, 0.25), (260, 0.01, 0.25), (800, 0.0, 0.25)] },
    // Reversals through zero speed, where the friction model is wrong, under
    // the voltage ceiling.
    Scenario {
        name: "S3_reversal",
        mode: MODE_FOC_TORQUE,
        points: &[(100, 0.03, 0.0), (300, -0.03, 0.0), (600, 0.03, 0.0), (900, 0.0, 0.0)],
    },
    // The estimator's consumer: a velocity step under the velocity method.
    Scenario { name: "S4_velstep", mode: MODE_FOC_VELOCITY, points: &[(100, 10.0, 0.0), (700, 0.0, 0.0)] },
    // A crawl under the velocity method: 0.5 rad/s held through the stop, the
    // regime of the friction model's ramp band and stiction.
    Scenario { name: "S5_crawl", mode: MODE_FOC_VELOCITY, points: &[(100, 0.5, 0.0)] },
];

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
        format!("{EST}.modelledTorque_Nm"),
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

/// The sequence as the widget would send it: the points, then the stop.
fn sequence(scenario: &Scenario) -> proto::Payload {
    let mut setpoints: Vec<_> = scenario
        .points
        .iter()
        .map(|&(t_ms, value, rate)| match scenario.mode {
            MODE_FOC_TORQUE => torque_setpoint(t_ms * 1000, value, rate),
            _ => speed_setpoint(t_ms * 1000, value, rate),
        })
        .collect();
    setpoints.push(stop_setpoint(DURATION_MS * 1000));
    command(scenario.mode, setpoints)
}

/// Run one scenario on one plant variant and write its capture; the time axis
/// starts at the Command's injection.
fn capture(scenario: &Scenario, inertia_factor: f64, coulomb_factor: f64) {
    let name = scenario.name;
    let base = plant_base();
    let plant_of = |fw: &voyant::Firmware| MotorParams {
        j_kg_m2: configured_inertia(fw) * inertia_factor,
        t_c_nm: base.t_c_nm * coulomb_factor,
        ..base
    };
    let mut sim = enabled_world_from(scenario.mode, plant_of, CurrentSenseParams::default());
    inject(&mut sim, &proto::envelope(1, sequence(scenario)));

    let columns = firmware_columns();
    let mut text = String::new();
    writeln!(text, "time_ms,{},plant.angle_rad,plant.velocity_radps", columns.join(",")).unwrap();
    let mut host = Host::new();
    let mut verdict = None;
    for cycle in 0..(RECORD_MS * CYCLES_PER_MS) {
        sim.run_for_us(GRID_US);
        let t_ms = f64::from(cycle + 1) / f64::from(CYCLES_PER_MS);
        write!(text, "{t_ms:.2}").unwrap();
        for path in &columns {
            write!(text, ",{:.7}", f64_at(&sim, path)).unwrap();
        }
        writeln!(text, ",{:.7},{:.7}", plant(&sim, "angle"), plant(&sim, "velocity")).unwrap();
        if verdict.is_none() && ((cycle + 1) % CYCLES_PER_MS == 0) {
            verdict = host.reply_to(&mut sim, 1).map(|reply| accepted(&reply));
        }
    }
    assert_eq!(verdict, Some(true), "{name}: the Command is accepted");
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

fn main() {
    let wanted: Vec<String> = std::env::args().skip(1).collect();
    let unknown: Vec<&String> = wanted.iter().filter(|w| !SCENARIOS.iter().any(|s| s.name == w.as_str())).collect();
    if !unknown.is_empty() {
        let names: Vec<&str> = SCENARIOS.iter().map(|s| s.name).collect();
        eprintln!("unknown scenario(s) {unknown:?}; known: {}", names.join(" "));
        std::process::exit(2);
    }
    write_provenance();
    for scenario in SCENARIOS.iter().filter(|s| wanted.is_empty() || wanted.iter().any(|w| w == s.name)) {
        for (inertia_factor, coulomb_factor) in variants() {
            capture(scenario, inertia_factor, coulomb_factor);
        }
    }
}
