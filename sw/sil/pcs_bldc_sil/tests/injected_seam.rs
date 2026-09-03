//! The `IO_bridge` injected current seam, end to end against the plant.
//!
//! `crest_sampling` proves the ADC samples the plant at the PWM crest, but it
//! decodes the raw counts itself and reconstructs phase W itself — so nothing
//! there exercises the firmware's own decode, its pair completion, or its KCL
//! reconstruction. This suite reads what `IO_bridge` actually published and
//! measures all three phases against the plant.

use pcs_bldc_sil::board::{board_with, fault_latched};
use pcs_bldc_sil::{Board, Sil};

mod common;
use common::{f64_at, plant, spin_up, u64_at, GRID_US};

/// The phase currents `IO_bridge` published, in the order `IO_bridge_phase_E`
/// declares them (U, V, then the derived W).
const PLANT_SIGNALS: [&str; 3] = ["phase_current_u", "phase_current_v", "phase_current_w"];

/// One phase's crest current as the firmware decoded it — not as the test would.
fn bridge_amps(sim: &Sil, phase: usize) -> f64 {
    f64_at(sim, &format!("IO_bridge_data.channels[0].current_amps[{phase}]"))
}

fn bridge_count(sim: &Sil, phase: usize) -> u64 {
    u64_at(sim, &format!("IO_bridge_data.channels[0].updateCount[{phase}]"))
}

fn bridge_stamp(sim: &Sil, phase: usize) -> u64 {
    u64_at(sim, &format!("IO_bridge_data.channels[0].sampleTime_us[{phase}]"))
}

// [test->fw~hal_adc_008~1]
// What the firmware publishes is the plant, on every phase — including the one
// it never sampled.
#[test]
fn the_bridge_publishes_crest_currents_matching_the_plant() {
    let Board { mut sim, .. } = board_with(Sil::options().grid_us(GRID_US), 0.8);
    spin_up(&mut sim);

    let steps = 800u64;
    let mut worst = [0.0f64; 3];
    for _ in 0..steps {
        sim.step().expect("engine step");
        for (phase, signal) in PLANT_SIGNALS.iter().enumerate() {
            let residual = (bridge_amps(&sim, phase) - plant(&sim, signal)).abs();
            worst[phase] = worst[phase].max(residual);
        }
    }
    eprintln!(
        "published residuals over {steps} periods: U {:.4} A, V {:.4} A, W {:.4} A",
        worst[0], worst[1], worst[2]
    );

    // The same floors crest_sampling measures against: 8.06 mA per LSB at
    // 0.1 V/A on 12 bits, and W sums two quantized channels so it carries two.
    assert!(worst[0] < 0.025, "published U tracks the plant ({:.4} A)", worst[0]);
    assert!(worst[1] < 0.025, "published V tracks the plant ({:.4} A)", worst[1]);
    assert!(worst[2] < 0.040, "derived W tracks the plant ({:.4} A)", worst[2]);
    assert!(!fault_latched(&sim), "no fault through the measurement window");
}

// The callback is registered on a real boot, and a pair completes every period:
// U and V each advance once per trigger, and W advances with them rather than
// stalling or double-counting.
#[test]
fn the_bridge_pairs_every_period_so_all_three_phases_advance() {
    let Board { mut sim, .. } = board_with(Sil::options().grid_us(GRID_US), 0.8);
    spin_up(&mut sim);

    let steps = 200u64;
    let before: Vec<u64> = (0..3).map(|p| bridge_count(&sim, p)).collect();
    let stamp_before = bridge_stamp(&sim, 0);
    for _ in 0..steps {
        sim.step().expect("engine step");
    }

    for phase in 0..3 {
        let advanced = bridge_count(&sim, phase) - before[phase];
        assert_eq!(
            advanced, steps,
            "phase {phase} published once per period, got {advanced} over {steps}"
        );
    }

    // Guards the assertion above against passing vacuously: a frozen time base
    // would stamp every sample identically, so everything would pair regardless.
    let elapsed = bridge_stamp(&sim, 0) - stamp_before;
    assert_eq!(
        elapsed,
        steps * GRID_US,
        "the stamps advance with the time base, not a frozen counter"
    );
}
