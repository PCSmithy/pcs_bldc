//! Stage 6 §A: the full board world on the control-rate (50 µs) grid. One engine
//! step is one PWM period, so the injected engine samples the plant's currents —
//! through the sense chain, on the shared pins — once per period, and the
//! completion interrupt (the SIL twin of the bench's `inj_cb`) dispatches once
//! per period with it.

use pcs_bldc_sil::board::{
    board_with, fault_latched, gate_operational, port, ALIGN_DUTY, GATE_BRINGUP_MS,
    GPIO_LEVEL_HIGH, GPIO_LEVEL_LOW, INPUT_LEVEL_PB10, MOTOR, VBUS_V,
};
use pcs_bldc_sil::{cid, vid, Board, CurrentSenseParams, MotorParams, Sil};

/// The PWM period at 20 kHz — the control-rate grid.
const GRID_US: u64 = 50;

/// The sim ADC's completion interrupt, resolved by name off the image's DWARF.
const COMPLETION_ISR: &str = "HW_ADC_private_completionDispatch";

/// One injected slot's raw counts, straight from firmware memory.
fn injected_count(sim: &Sil, ch: usize) -> u64 {
    let path = format!("HW_ADC_data.channelData[{ch}].injectedCounts[0]");
    sim.fw()
        .read_cvar(&path)
        .as_u64()
        .unwrap_or_else(|| panic!("{path} reads as an unsigned count"))
}

/// Decode an injected slot the way the firmware's phase decode would: counts →
/// pin volts → amps through the sense transfer.
fn injected_amps(sim: &Sil, ch: usize, params: &CurrentSenseParams) -> f64 {
    let volts = (injected_count(sim, ch) as f64 / 4095.0) * 3.3;
    (volts - params.phase_bias_v) / params.phase_gain_v_per_a
}

fn plant(sim: &Sil, local: &str) -> f64 {
    sim.read_f64(&vid(MOTOR, local))
}

fn tap_button(sim: &mut Sil) {
    sim.write(&cid(INPUT_LEVEL_PB10), GPIO_LEVEL_LOW).expect("button press");
    sim.run_for_ms(30);
    sim.write(&cid(INPUT_LEVEL_PB10), GPIO_LEVEL_HIGH).expect("button release");
    sim.run_for_ms(30);
    sim.run_for_ms(320); // APP_USERCONTROLS_DOUBLE_TAP_WINDOW_MS plus slack
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
    assert!(
        port(&sim, "PWM_U_enabled") == 1.0 && ALIGN_DUTY > 0.0,
        "alignment pattern is driving"
    );

    // The injected slots track the plant through the sense chain, per period:
    // sample instants land on the driven current, not a stale pre-arm value.
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
