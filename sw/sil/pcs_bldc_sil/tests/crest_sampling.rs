//! Stage 6 §A: the full board world on the control-rate (50 µs) grid. One engine
//! step is one PWM period, so the injected engine samples the plant's currents —
//! through the sense chain, on the shared pins — once per period, and the
//! completion interrupt (the SIL twin of the bench's `inj_cb`) dispatches once
//! per period with it. On a finer grid, the duties that interrupt writes reach
//! the bridge at the next crest, through the compare preload.

use pcs_bldc_sil::board::{
    board_with, fault_latched, gate_operational, port, align_duty, align_dwell_ms, DIAL, GATE_BRINGUP_MS, VBUS_V,
};
use pcs_bldc_sil::{vid, Board, CurrentSenseParams, MotorParams, Sil};

mod common;
use common::{
    assert_status, f64_at, plant, select_method, spin_up, tap_button, u64_at, ADC_BUSY, GRID_US, MODE_SINUSOIDAL,
    MODE_SIX_STEP_TRAP,
};

/// The sim ADC's completion interrupt, resolved by name off the image's DWARF.
const COMPLETION_ISR: &str = "HW_ADC_sim_completionDispatch";

/// One injected slot's raw counts, straight from firmware memory.
fn injected_count(sim: &Sil, ch: usize) -> u64 {
    u64_at(sim, &format!("HW_ADC_data.channelData[{ch}].injectedCounts[0]"))
}

/// Decode an injected slot the way the firmware's phase decode would: counts →
/// pin volts at the channel's oversampled scale → amps through the sense transfer.
fn injected_amps(sim: &Sil, ch: usize, params: &CurrentSenseParams) -> f64 {
    let volts_per_count = f64_at(sim, &format!("HW_ADC_data.channelData[{ch}].injectedVoltsPerCount"));
    let volts = injected_count(sim, ch) as f64 * volts_per_count;
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
    select_method(&mut sim, MODE_SIX_STEP_TRAP);
    tap_button(&mut sim);
    sim.run_for_ms(200); // partway into the dwell — current settled, still aligning
    let expected_a = (align_duty(&sim) * VBUS_V) / (1.5 * MotorParams::default().r_ohm);   // U against V and W returning
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


// The fw~hal_adc_010 acceptance window: at least 10 s of sustained operation at
// the 20 kHz trigger rate with regular sampling passes running. The regular DMA
// path runs on the real 1 ms task beside the injected stream the whole way.
// [test->fw~hal_adc_010~1]
#[test]
fn injected_completions_equal_timer_events_over_ten_seconds() {
    let Board { mut sim, fw_member, .. } = board_with(Sil::options().grid_us(GRID_US), 0.8);
    sim.run_for_ms(GATE_BRINGUP_MS);
    assert!(!fault_latched(&sim), "no fault through boot");

    let completion = fw_member
        .borrow()
        .find_isr(COMPLETION_ISR)
        .expect("completion interrupt registered");
    let before = fw_member.borrow().isr_dispatch_count_of(completion);
    let steps = 200_000u64; // 10 s of 50 µs PWM periods
    for _ in 0..steps {
        sim.step().expect("engine step");
    }
    let after = fw_member.borrow().isr_dispatch_count_of(completion);
    assert_eq!(
        after - before,
        steps,
        "completed injected sequences equal trigger events one-to-one over 10 s"
    );

    // The concurrency condition held: the regular DMA path is still cycling on
    // both ADCs — at this exact-tick boundary a healthy channel's pass is in
    // flight (FAULT is the failure signal).
    for ch in 0..2 {
        assert_status(
            &sim,
            &format!("HW_ADC_data.channelData[{ch}].status"),
            &ADC_BUSY,
            "regular DMA path cycling beside the injected stream",
        );
    }
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

    // Coexistence: the regular 1 ms sequencer keeps cycling on the shared
    // pins throughout — at this exact-tick boundary a healthy channel's pass
    // is in flight (FAULT is the failure signal).
    for ch in 0..2 {
        assert_status(
            &sim,
            &format!("HW_ADC_data.channelData[{ch}].status"),
            &ADC_BUSY,
            "regular path cycling beside the injected stream",
        );
    }
}

/// The PWM timer's state and configuration (TIM1, peripheral 0; PWM_U is OC unit 0).
const TIM: &str = "HW_TIM_data.peripheralData[0]";
const TIM_CONFIG: &str = "HW_TIM_peripheralConfig[0]";

/// Whether an advance of `counts` from counter position `from` lands on position
/// `target` of a `cycle`-long counting cycle.
fn lands_on(from: u64, counts: u64, target: u64, cycle: u64) -> bool {
    let distance = match (target + cycle - from) % cycle {
        0 => cycle,
        d => d,
    };
    distance <= counts
}

// [test->fw~hal_tim_009~1]
#[test]
fn the_first_update_lands_on_the_first_crest_after_the_start() {
    // TIM1 counts center-aligned with its update on the period reload: a compare
    // written into the preload register before the counter's first crest stays off
    // the output through the up-count and reaches it at that crest.
    const FINE_US: u64 = 5;
    let mut sim = Sil::options().grid_us(FINE_US).build();
    let fwm = sim.load_firmware(pcs_bldc_sil::SOURCE);
    sim.add_member(fwm);
    let period = u64_at(&sim, &format!("{TIM_CONFIG}.period"));
    let counts = FINE_US * u64_at(&sim, &format!("{TIM_CONFIG}.countsPerUs"));
    assert_eq!(u64_at(&sim, &format!("{TIM}.position")), 0, "the counter starts at its first cycle's start");
    let half = period / 2;
    sim.fw().write_cvar(&format!("{TIM}.compare[0]"), &voyant::Value::U32(half as u32));
    let mut position = 0;
    let mut latched_at = None;
    for step in 0..(2 * period / counts) {
        sim.step().expect("engine step");
        let crest = lands_on(position, counts, period, 2 * period);
        position = u64_at(&sim, &format!("{TIM}.position"));
        let active = u64_at(&sim, &format!("{TIM}.activeCompare[0]"));
        if active == half {
            latched_at.get_or_insert((step, crest));
        }
    }
    let output = (port(&sim, "PWM_U_duty") * period as f64).round() as u64;
    println!("crest_sampling: a compare written at the start latched at step {latched_at:?} of {FINE_US} us");
    assert_eq!(latched_at.map(|(_, crest)| crest), Some(true), "the first update is the first crest");
    assert_eq!(latched_at.map(|(step, _)| (step + 1) * counts), Some(period), "at the first period reload, not before");
    assert_eq!(output, half, "and the output follows it");
}

// [test->fw~hal_tim_009~1]
#[test]
fn a_duty_written_in_the_cycle_interrupt_drives_the_output_from_the_next_crest() {
    // Sinusoidal drive writes new duties every cycle from the injected-completion
    // interrupt, which follows the trigger just before the crest. On a 5 us grid
    // the center-aligned counter's crest and valley land in different steps: a
    // written compare reads back at once and reaches the PWM output at the first
    // crest after the write — a period on — never at a valley.
    const FINE_US: u64 = 5;
    let Board { mut sim, .. } = board_with(Sil::options().grid_us(FINE_US), 0.0);
    sim.run_for_ms(GATE_BRINGUP_MS);
    select_method(&mut sim, MODE_SINUSOIDAL);
    tap_button(&mut sim);
    sim.run_for_ms(align_dwell_ms(&sim) + 100);
    let mut deg = 0.0;
    while deg < 90.0 {
        deg += 10.0;
        sim.write(&vid(DIAL, "angle[deg]"), deg).expect("turn the dial");
        sim.run_for_ms(20);
    }
    sim.run_for_ms(100);

    let period = u64_at(&sim, &format!("{TIM_CONFIG}.period"));
    let cycle = 2 * period;
    let counts = FINE_US * u64_at(&sim, &format!("{TIM_CONFIG}.countsPerUs"));
    let read = |sim: &Sil| {
        (
            u64_at(sim, &format!("{TIM}.position")),
            u64_at(sim, &format!("{TIM}.compare[0]")),
            u64_at(sim, &format!("{TIM}.activeCompare[0]")),
            (port(sim, "PWM_U_duty") * period as f64).round() as u64,
        )
    };

    let (mut position, mut written, _, mut output) = read(&sim);
    let mut pending: Option<(u64, u64)> = None;   // (step written, value)
    let (mut writes, mut latches) = (0u32, 0u32);
    let mut delays_us = Vec::new();
    for step in 0..(20 * 1000 / FINE_US) {
        sim.step().expect("engine step");
        let (now_position, now_written, active, now_output) = read(&sim);
        let crest = lands_on(position, counts, period, cycle);
        let valley = lands_on(position, counts, 0, cycle);
        if now_output != output {
            assert!(crest, "step {step}: the output changes only at a crest (valley {valley})");
            // The first change can latch a write from before the window.
            if writes > 0 {
                let (written_at, value) = pending.take().expect("an output change follows a write");
                assert_eq!(now_output, value, "step {step}: the output takes the written compare");
                delays_us.push((step - written_at) * FINE_US);
                latches += 1;
            }
        } else if crest && (writes > 0) {
            assert!(pending.is_none(), "step {step}: a crest after a write puts it on the output");
        }
        if now_written != written {
            writes += 1;
            assert!(pending.is_none(), "step {step}: the previous write reached the output first");
            assert_ne!(active, now_written, "step {step}: the written compare reads back before it is active");
            pending = Some((step, now_written));
        }
        (position, written, output) = (now_position, now_written, now_output);
    }
    let (min, max) = (delays_us.iter().min().copied(), delays_us.iter().max().copied());
    println!("crest_sampling: {writes} compare writes, {latches} reached the output, each {min:?}..{max:?} us after its write step");
    assert!(latches > 300, "the drive rewrites the duty every period: {latches} latches over 20 ms");
    let pwm_period_us = cycle / u64_at(&sim, &format!("{TIM_CONFIG}.countsPerUs"));
    assert!(delays_us.iter().all(|&d| d == pwm_period_us), "every write reaches the output one {pwm_period_us} us PWM period on: {min:?}..{max:?} us");
    assert!(!fault_latched(&sim), "no fault through the run");
}
