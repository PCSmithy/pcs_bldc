//! Stage 6 §A: the full board world on the control-rate (50 µs) grid. One engine
//! step is one PWM period, so the injected engine samples the plant's currents —
//! through the sense chain, on the shared pins — once per period, and the
//! completion interrupt (the SIL twin of the bench's `inj_cb`) dispatches once
//! per period with it.

use pcs_bldc_sil::board::{
    board_with, fault_latched, gate_operational, port, ALIGN_DUTY, GATE_BRINGUP_MS, VBUS_V,
};
use pcs_bldc_sil::{Board, CurrentSenseParams, MotorParams, Sil};

mod common;
use common::{assert_status, plant, spin_up, tap_button, u64_at, ADC_OK, GRID_US};

/// The sim ADC's completion interrupt, resolved by name off the image's DWARF.
const COMPLETION_ISR: &str = "HW_ADC_sim_completionDispatch";

/// One injected slot's raw counts, straight from firmware memory.
fn injected_count(sim: &Sil, ch: usize) -> u64 {
    u64_at(sim, &format!("HW_ADC_data.channelData[{ch}].injectedCounts[0]"))
}

/// Decode an injected slot the way the firmware's phase decode would: counts →
/// pin volts → amps through the sense transfer.
fn injected_amps(sim: &Sil, ch: usize, params: &CurrentSenseParams) -> f64 {
    let volts = (injected_count(sim, ch) as f64 / 4095.0) * 3.3;
    (volts - params.phase_bias_v) / params.phase_gain_v_per_a
}


// [test->fw~hal_adc_003~1]
// [test->fw~hal_adc_008~1]
#[test]
fn the_board_world_samples_the_plant_once_per_period_on_the_fine_grid() {
    let params = CurrentSenseParams::default();
    let wall = std::time::Instant::now();

    let Board { mut sim, fw_member, .. } = board_with(Sil::options().grid_us(GRID_US), 0.8);
    sim.run_for_ms(GATE_BRINGUP_MS);
    assert!(gate_operational(&sim), "gate driver operational after boot");
    assert!(!fault_latched(&sim), "no fault through boot");

    let completion = fw_member
        .borrow()
        .find_isr(COMPLETION_ISR)
        .expect("the pended completion interrupt registered at HW_ADC_init");

    // Cadence, bridge dark: exactly one completion per engine step — one per PWM
    // period, the bench's 20 kHz with no idle undercount to model.
    let before = fw_member.borrow().isr_dispatch_count_of(completion);
    let steps = 20 * 50; // 50 ms
    for _ in 0..steps {
        sim.step().expect("engine step");
    }
    let after = fw_member.borrow().isr_dispatch_count_of(completion);
    assert_eq!(after - before, steps, "one completion per PWM period, bridge dark");

    // Bridge dark: no winding current, so both injected slots read the sense bias.
    assert!(
        injected_amps(&sim, 0, &params).abs() < 0.01,
        "U slot reads zero current at rest"
    );
    assert!(
        injected_amps(&sim, 1, &params).abs() < 0.01,
        "V slot reads zero current at rest"
    );

    // Arm: the alignment dwell drives a known steady current through U and V.
    tap_button(&mut sim);
    sim.run_for_ms(200); // partway into the dwell — current settled, still aligning
    let expected_a = (ALIGN_DUTY * VBUS_V) / (2.0 * MotorParams::default().r_ohm);
    assert_eq!(port(&sim, "PWM_U_enabled"), 1.0, "alignment pattern is driving");

    // The injected slots track the plant through the sense chain: the samples
    // carry the driven current, not a stale pre-arm value.
    for phase in [(0usize, "phase_current_u"), (1usize, "phase_current_v")] {
        let (ch, sig) = phase;
        let inj = injected_amps(&sim, ch, &params);
        let truth = plant(&sim, sig);
        assert!(
            (inj - truth).abs() < 0.02,
            "{sig}: injected decodes {inj:.4} A, plant carries {truth:.4} A"
        );
    }
    let i_u = injected_amps(&sim, 0, &params);
    assert!(
        (i_u - expected_a).abs() < (0.15 * expected_a),
        "alignment current magnitude sane: {i_u:.3} A vs ~{expected_a:.3} A"
    );

    // Per-period freshness while driving: the counter still tracks steps 1:1.
    let before = fw_member.borrow().isr_dispatch_count_of(completion);
    for _ in 0..100 {
        sim.step().expect("engine step");
    }
    let after = fw_member.borrow().isr_dispatch_count_of(completion);
    assert_eq!(after - before, 100, "one completion per period while driving");

    eprintln!(
        "fine-grid board world: {:.2} s sim in {:.2} s wall",
        sim.now_us() as f64 / 1e6,
        wall.elapsed().as_secs_f64()
    );
}


// [test->fw~hal_adc_003~1]
// [test->fw~hal_adc_008~1]
#[test]
fn north_star_injected_matches_the_plant_every_period_while_spinning() {
    let params = CurrentSenseParams::default();
    let Board { mut sim, fw_member, .. } = board_with(Sil::options().grid_us(GRID_US), 0.8);
    spin_up(&mut sim);

    // Measurement window: every PWM period for 40 ms (~several electrical
    // cycles). Each step's injected slots are compared against the plant's
    // winding currents at that step — the residual bounds the sense chain's
    // error. (Where in the period the sample instant lands is asserted at the
    // unit level, in test_HW_ADC.c::test_falling_edge_selects_the_down_crossing.)
    let completion = fw_member
        .borrow()
        .find_isr(COMPLETION_ISR)
        .expect("completion interrupt registered");
    let c0 = fw_member.borrow().isr_dispatch_count_of(completion);

    // Same-step comparison: the zero-latency sense chain delivers the plant's
    // state into the pins before the member's timebase advances, so the crest
    // sample reads THIS period's currents. Residual floor is the 12-bit
    // quantization (~8 mA); the real chain's ~2-3 µs (INA240 + RC) rounds to
    // zero at this grid.
    let steps = 800u64;
    let (mut max_uv, mut max_w) = (0.0f64, 0.0f64);
    for _ in 0..steps {
        sim.step().expect("engine step");
        let now = [
            plant(&sim, "phase_current_u"),
            plant(&sim, "phase_current_v"),
            plant(&sim, "phase_current_w"),
        ];
        for (ch, truth) in now.iter().take(2).enumerate() {
            max_uv = max_uv.max((injected_amps(&sim, ch, &params) - truth).abs());
        }
        // Two-shunt derivation: the third phase reconstructed from the two
        // sampled ones, against the plant's own i_w.
        let w = -(injected_amps(&sim, 0, &params) + injected_amps(&sim, 1, &params));
        max_w = max_w.max((w - now[2]).abs());
    }
    let cadence = fw_member.borrow().isr_dispatch_count_of(completion) - c0;
    eprintln!("residuals over {steps} periods: U/V {max_uv:.4} A, derived-W {max_w:.4} A");

    assert_eq!(cadence, steps, "one completion per PWM period through commutation");
    // Thresholds are the measured maximum plus quantization headroom (8.06 mA per
    // LSB at 0.1 V/A on 12 bits), so a cross-platform ULP flip cannot fail CI:
    // U/V 13.6 mA + 1 LSB; derived-W 16.8 mA + 2 LSB (two quantized channels summed).
    assert!(max_uv < 0.025, "injected tracks the plant per period ({max_uv:.4} A)");
    assert!(max_w < 0.040, "derived W tracks the plant ({max_w:.4} A)");

    // Coexistence: the regular 1 ms sequencer keeps running on the shared
    // pins throughout — its per-pass status is still OK on both ADCs.
    for ch in 0..2 {
        assert_status(
            &sim,
            &format!("HW_ADC_data.status[{ch}]"),
            &ADC_OK,
            "regular path healthy beside the injected stream",
        );
    }
}
