//! Zero-current offset tracking (fw~io_bridge_009) on the board world: a sense
//! front end whose phase bias sits 10 mV off the firmware's decode reads 100 mA
//! with no current flowing; the bridge learns that offset over the dark run
//! before an enable, so the current loop regulates the true winding current, and
//! a six-step crawl relearns it from its floating phases.

mod common;

use common::{
    enabled_world_from, f64_at, plant, select_method, tap_button, u64_at, GRID_US, MODE_FOC_TORQUE, MODE_SIX_STEP_TRAP,
};
use pcs_bldc_sil::board::{board_from, fault_latched, align_dwell_ms, DIAL, GATE_BRINGUP_MS};
use pcs_bldc_sil::{vid, CurrentSenseParams, MotorParams, Sil, SilOptions};

/// The plant's phase bias, 10 mV above the board's 1.65 V midpoint.
const PLANT_PHASE_BIAS_V: f64 = 1.66;

/// The sensed phases: U and V carry injected samples, W is derived.
const SENSED: [usize; 2] = [0, 1];

fn sense() -> CurrentSenseParams {
    CurrentSenseParams { phase_bias_v: PLANT_PHASE_BIAS_V, ..CurrentSenseParams::default() }
}

/// The offset a dark phase reads: the plant's bias through the firmware's own decode.
fn expected_offset_a(sim: &Sil, phase: usize) -> f64 {
    let config = format!("IO_bridge_channelConfig[0].phase[{phase}].currentSense");
    (PLANT_PHASE_BIAS_V - f64_at(sim, &format!("{config}.bias_V"))) / f64_at(sim, &format!("{config}.voltsPerUnit"))
}

fn learned_offset_a(sim: &Sil, phase: usize) -> f64 {
    f64_at(sim, &format!("IO_bridge_data.channels[0].phaseCurrentZeroOffset_amps[{phase}]"))
}

/// The phase current the bridge reports: its latest sample less the offset.
fn reported_a(sim: &Sil, phase: usize) -> f64 {
    f64_at(sim, &format!("IO_bridge_data.channels[0].measuredPhaseCurrent_amps[{phase}]")) - learned_offset_a(sim, phase)
}

// [test->fw~io_bridge_009~1]
#[test]
fn the_dark_run_offset_zeroes_the_regulated_current() {
    // The gate bring-up keeps the bridge dark far past the 50 ms minimum run; the
    // enable latches the offset, and under the torque method at a zero target (the
    // cogging feed-forward the only current asked) the bridge reports the winding's
    // true current. Without the offset it would read 100 mA off.
    let mut sim = enabled_world_from(MODE_FOC_TORQUE, |_| MotorParams::default(), sense());
    sim.run_for_ms(50);
    let (mut true_sum, mut reported_sum) = ([0.0_f64; 2], [0.0_f64; 2]);
    let samples = 20 * 50;
    for _ in 0..samples {
        sim.run_for_us(GRID_US);
        for (k, &phase) in SENSED.iter().enumerate() {
            true_sum[k] += plant(&sim, ["phase_current_u", "phase_current_v"][k]);
            reported_sum[k] += reported_a(&sim, phase);
        }
    }
    for (k, &phase) in SENSED.iter().enumerate() {
        let (truth, reported) = (true_sum[k] / f64::from(samples), reported_sum[k] / f64::from(samples));
        let (learned, expected) = (learned_offset_a(&sim, phase), expected_offset_a(&sim, phase));
        println!(
            "zero_offset: phase {phase}: offset learned {:+.1} mA (bias reads {:+.1} mA); over 50 ms at a zero target \
             the winding carries {:+.1} mA, the bridge reports {:+.1} mA",
            1e3 * learned, 1e3 * expected, 1e3 * truth, 1e3 * reported
        );
        assert!((learned - expected).abs() < 0.010, "phase {phase}: the dark run's offset is the bias reading: {learned:+.4} A");
        assert!((reported - truth).abs() < 0.010, "phase {phase}: the reported current is the winding's within 10 mA: {reported:+.4} A");
    }
    assert!(!fault_latched(&sim), "no fault");
}

// [test->fw~io_bridge_009~1]
#[test]
fn a_six_step_crawl_relearns_the_offset_from_its_floating_phases() {
    // A crawl slow enough that a phase floats for longer than the minimum run in
    // the sectors it sits out: the offset latched at the end of such a float is the
    // bias reading, the freewheel current at its first samples decayed out of the
    // average.
    let mut sim = board_from(SilOptions::default().grid_us(GRID_US), 0.0, |_| MotorParams::default(), sense()).sim;
    sim.run_for_ms(GATE_BRINGUP_MS);
    select_method(&mut sim, MODE_SIX_STEP_TRAP);
    tap_button(&mut sim);
    sim.run_for_ms(align_dwell_ms(&sim) + 100);
    let mut deg = 0.0;
    while deg < CRAWL_DIAL_DEG {
        deg += 2.0;
        sim.write(&vid(DIAL, "angle[deg]"), deg).expect("turn the dial");
        sim.run_for_ms(20);
    }
    // Clear the dark run's offset so only a float can put it back.
    for phase in SENSED {
        sim.fw().write_cvar(&format!("IO_bridge_data.channels[0].phaseCurrentZeroOffset_amps[{phase}]"), &voyant::Value::F32(0.0));
    }
    let mut floats = [0u32; 2];
    let mut longest = [0u32; 2];
    let mut run = [0u32; 2];
    let mut was_enabled = [true; 2];
    let mut travel = common::Unwrapped::new(&sim);
    for _ in 0..(CRAWL_MS * 20) {
        sim.run_for_us(GRID_US);
        travel.update(&sim);
        for (k, name) in ["PWM_U_enabled", "PWM_V_enabled"].iter().enumerate() {
            let enabled = pcs_bldc_sil::board::port(&sim, name) == 1.0;
            if was_enabled[k] && !enabled {
                floats[k] += 1;
            }
            run[k] = if enabled { 0 } else { run[k] + 1 };
            longest[k] = longest[k].max(run[k]);
            was_enabled[k] = enabled;
        }
    }
    let travelled = travel.update(&sim);
    let min_run = u64_at(&sim, "IO_bridge_channelConfig[0].zeroOffset.minSamples") as u32;
    for (k, &phase) in SENSED.iter().enumerate() {
        let (learned, expected) = (learned_offset_a(&sim, phase), expected_offset_a(&sim, phase));
        println!(
            "zero_offset: crawl {travelled:+.3} rad over {CRAWL_MS} ms: phase {phase} floated {} times, the longest {} cycles \
             (minimum run {min_run}); offset {:+.1} mA against the bias reading {:+.1} mA",
            floats[k], longest[k], 1e3 * learned, 1e3 * expected
        );
        assert!(longest[k] > min_run, "phase {phase} floated past the minimum run: {} cycles", longest[k]);
        assert!((learned - expected).abs() < 0.010, "phase {phase}: a float relearns the bias reading within 10 mA: {learned:+.4} A");
    }
    assert!(!fault_latched(&sim), "no fault through the crawl");
}

/// The crawl: a dial turn small enough that each sector outlasts the 50 ms run.
const CRAWL_DIAL_DEG: f64 = 12.0;
const CRAWL_MS: u64 = 2000;
