//! Stage 6 §A: the full board world on the control-rate (50 µs) grid. One engine
//! step is one PWM period, so the injected engine samples the plant's currents —
//! through the sense chain, on the shared pins — once per period, and the
//! completion interrupt (the SIL twin of the bench's `inj_cb`) dispatches once
//! per period with it.

use pcs_bldc_sil::board::{
    board_with, fault_latched, gate_operational, port, ALIGN_DUTY, ALIGN_DWELL_MS, DIAL,
    GATE_BRINGUP_MS, GPIO_LEVEL_HIGH, GPIO_LEVEL_LOW, INPUT_LEVEL_PB10, MOTOR, VBUS_V,
};
use pcs_bldc_sil::{cid, vid, Board, CurrentSenseParams, MotorParams, Sil};

/// The PWM period at 20 kHz — the control-rate grid.
const GRID_US: u64 = 50;

/// The sim ADC's completion interrupt, resolved by name off the image's DWARF.
const COMPLETION_ISR: &str = "HW_ADC_sim_completionDispatch";

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

/// Boot, arm, let the alignment dwell finish, ramp the dial to half command,
/// and settle into closed-loop commutation.
fn spin_up(sim: &mut Sil) {
    sim.run_for_ms(GATE_BRINGUP_MS);
    tap_button(sim);
    sim.run_for_ms(ALIGN_DWELL_MS + 100);
    assert!(
        sim.read_bool(&cid("app_motorControl_data.channels[0].isAligned")),
        "alignment latched before the spin"
    );
    let mut deg = 0.0;
    while deg < 90.0 {
        deg += 10.0;
        sim.write(&vid(DIAL, "angle[deg]"), deg).expect("turn the dial");
        sim.run_for_ms(20);
    }
    sim.run_for_ms(300);
    let velocity = plant(sim, "velocity");
    assert!(velocity.abs() > 2.0, "the shaft is spinning, got {velocity:.2} rad/s");
    assert!(!fault_latched(sim), "no fault through the spin-up");
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
    // winding currents at that step and at the previous one — the residual
    // pins down where in the step the sample instant lives.
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
        for ch in 0..2usize {
            max_uv = max_uv.max((injected_amps(&sim, ch, &params) - now[ch]).abs());
        }
        // Two-shunt derivation: the third phase reconstructed from the two
        // sampled ones, against the plant's own i_w.
        let w = -(injected_amps(&sim, 0, &params) + injected_amps(&sim, 1, &params));
        max_w = max_w.max((w - now[2]).abs());
    }
    let cadence = fw_member.borrow().isr_dispatch_count_of(completion) - c0;
    eprintln!("residuals over {steps} periods: U/V {max_uv:.4} A, derived-W {max_w:.4} A");

    assert_eq!(cadence, steps, "one completion per PWM period through commutation");
    assert!(max_uv < 0.025, "injected tracks the plant per period ({max_uv:.4} A)");
    assert!(max_w < 0.040, "derived W tracks the plant ({max_w:.4} A)");

    // Coexistence: the regular 1 ms sequencer keeps running on the shared
    // pins throughout — its per-pass status is still OK on both ADCs.
    for ch in 0..2 {
        let path = format!("HW_ADC_data.status[{ch}]");
        let got = match sim.fw().read_cvar(&path) {
            voyant::Value::Enum(name) => name,
            other => panic!("{path} reads as an enum, got {other:?}"),
        };
        assert!(
            got == "HW_ADC_CONVERSION_STATUS_OK" || got == "<2>",
            "regular path healthy beside the injected stream: {path} = {got}"
        );
    }
}

/// Trace-generation variant: the same spin with the cvar mirror ungated, so
/// firmware statics land in the historian at every dispatching step instead of
/// the 1 ms sweep cadence. Not an assert suite — run it explicitly to dump a
/// dense MDF:
///   PCS_SIL_TRACE_DIR=build/traces cargo test --release --test crest_sampling \
///     -- --ignored north_star_trace
#[test]
#[ignore = "trace generation: set PCS_SIL_TRACE_DIR and run with --ignored"]
fn north_star_trace_ungated_mirror() {
    let Board { mut sim, .. } =
        board_with(Sil::options().grid_us(GRID_US).sweep_period_us(0), 0.8);
    spin_up(&mut sim);
    sim.run_for_ms(40); // the observation window the assert suite measures
}
