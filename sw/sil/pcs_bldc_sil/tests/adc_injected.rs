//! Hardware-triggered injected sampling: the sim twin of the TIM1 TRGO2 → ADC1/ADC2
//! chain, asserted white-box against the real firmware. The sim board config arms
//! both channels (rising edge, one slot each on pins IN6/IN7, oversampled as the
//! stm32g4 config is); triggers land during the TIM advance and completions drain
//! through the SIL_irq service, so status, counts, and cadence are all observable
//! from firmware memory. Every scale is the channel config's, read out of the image.

mod common;
use common::{assert_status, bool_at, booted, f64_at, u64_at, ADC_OK};
use pcs_bldc_sil::{vid, Sil, SOURCE};

const U_PIN_PORT: &str = "ADC1_IN6";
const V_PIN_PORT: &str = "ADC2_IN7";

/// The regular path's count of a driven pin voltage on ADC channel `ch`: the
/// configured reference over the configured resolution, rounded.
fn counts_of(sim: &Sil, ch: usize, volts: f64) -> u64 {
    let vref = f64_at(sim, &format!("HW_ADC_channelConfig[{ch}].vref"));
    let full_scale = (1u64 << u64_at(sim, &format!("HW_ADC_channelConfig[{ch}].numBits"))) - 1;
    ((volts / vref) * full_scale as f64 + 0.5) as u64
}

/// The injected slot's count of the same voltage: the oversampling sum of
/// `ratio` conversions shifted right `shift` bits, as slot 0 is configured.
fn injected_counts_of(sim: &Sil, ch: usize, volts: f64) -> u64 {
    let slot = format!("HW_ADC_channelConfig[{ch}].injectedInputs[0]");
    let (ratio, shift) = if bool_at(sim, &format!("{slot}.oversamplingEnabled")) {
        (u64_at(sim, &format!("{slot}.oversamplingRatio")), u64_at(sim, &format!("{slot}.oversamplingShift")))
    } else {
        (1, 0)
    };
    (counts_of(sim, ch, volts) * ratio) >> shift
}

fn injected_count(sim: &Sil, ch: usize) -> u64 {
    u64_at(sim, &format!("HW_ADC_data.channelData[{ch}].injectedCounts[0]"))
}

/// The pin voltage the bridge decoded off phase `phase`'s injected slot — its
/// own sense transfer inverted on the amps it reports before any offset.
fn bridge_decoded_volts(sim: &Sil, phase: usize) -> f64 {
    let sense = format!("IO_bridge_channelConfig[0].phase[{phase}].currentSense");
    let amps = f64_at(sim, &format!("IO_bridge_data.channels[0].measuredPhaseCurrent_amps[{phase}]"));
    f64_at(sim, &format!("{sense}.bias_V")) + (amps * f64_at(sim, &format!("{sense}.voltsPerUnit")))
}

// [test->fw~hal_adc_003~1]
#[test]
fn injected_samples_the_shared_pin_and_completes() {
    let mut sim = booted(1);

    // The status walked BUSY → OK within the first millisecond: the trigger chain
    // fired and the completion service drained it on both channels.
    for ch in 0..2 {
        assert_status(
            &sim,
            &format!("HW_ADC_data.channelData[{ch}].injectedConversionStatus"),
            &ADC_OK,
            &format!("channel {ch} injected conversion completed"),
        );
    }
    assert_eq!(
        u64_at(&sim, "HW_ADC_data.channelData[0].pendingCompletions"),
        0,
        "every queued conversion has been drained"
    );

    // A driven pin is sampled at the trigger instant — and it is the SAME pin the
    // regular path reads (IN6/IN7), as on silicon.
    const U_VOLTS: f64 = 1.65;
    const V_VOLTS: f64 = 0.9;
    sim.write(&vid(SOURCE, U_PIN_PORT), U_VOLTS).expect("drive U pin");
    sim.write(&vid(SOURCE, V_PIN_PORT), V_VOLTS).expect("drive V pin");
    sim.run_for_ms(2);

    assert_eq!(injected_count(&sim, 0), injected_counts_of(&sim, 0, U_VOLTS), "U slot quantizes the pin");
    assert_eq!(injected_count(&sim, 1), injected_counts_of(&sim, 1, V_VOLTS), "V slot quantizes the pin");
    assert_eq!(
        u64_at(&sim, "HW_ADC_data.channelData[0].counts[6]"),
        counts_of(&sim, 0, U_VOLTS),
        "regular and injected read the same physical pin"
    );
}

// [test->fw~hal_adc_003~1]
// [test->fw~hal_adc_008~1]
#[test]
fn one_conversion_per_pwm_period_on_the_fine_grid() {
    // On the 50 µs grid each engine step is one PWM period: the up-crossing lands
    // once per step, and the completion service drains it in the same step — so a
    // per-step pin change is captured per step. This is the 20 kHz cadence plus the
    // rising-edge-only selection (both edges would sample twice per step).
    let mut sim = Sil::options().grid_us(50).build();
    let fwm = sim.load_firmware(SOURCE);
    sim.add_member(fwm);
    sim.run_for_ms(1);

    // Inputs sync into the C-side ports before the timebase advances, so the
    // very next step's trigger samples a fresh table write — zero-latency
    // delivery, per period.
    for (i, volts) in [0.5, 1.1, 2.2, 3.0].iter().enumerate() {
        sim.write(&vid(SOURCE, U_PIN_PORT), *volts).expect("drive U pin");
        sim.step().expect("engine step");
        assert_eq!(
            injected_count(&sim, 0),
            injected_counts_of(&sim, 0, *volts),
            "period {i}: this period's trigger captures the new pin voltage"
        );
    }
}

// [test->fw~hal_adc_006~1]
#[test]
fn an_oversampled_injected_count_reads_back_at_the_pin_voltage() {
    // The board oversamples the injected slots: the count is the sum of the
    // configured ratio of conversions, shifted, and the voltage the bridge reads
    // through HW_ADC_getInjectedVolts is the pin's, full scale included — the
    // reference voltage reads as itself, one LSB of the 12-bit conversion at most.
    let mut sim = booted(1);
    let vref = f64_at(&sim, "HW_ADC_channelConfig[0].vref");
    let lsb = vref / ((1u64 << u64_at(&sim, "HW_ADC_channelConfig[0].numBits")) - 1) as f64;
    for (u_volts, v_volts) in [(vref, 1.0), (0.4, f64_at(&sim, "HW_ADC_channelConfig[1].vref"))] {
        sim.write(&vid(SOURCE, U_PIN_PORT), u_volts).expect("drive U pin");
        sim.write(&vid(SOURCE, V_PIN_PORT), v_volts).expect("drive V pin");
        sim.run_for_ms(2);
        for (ch, volts) in [(0usize, u_volts), (1usize, v_volts)] {
            let count = injected_count(&sim, ch);
            assert_eq!(count, injected_counts_of(&sim, ch, volts), "channel {ch}: the oversampled sum of {volts} V");
            assert!(count > counts_of(&sim, ch, volts), "channel {ch}: the count carries the oversampling");
            let read = bridge_decoded_volts(&sim, ch);
            println!("adc_injected: channel {ch} at {volts:.3} V: count {count}, read back {read:.5} V");
            assert!((read - volts).abs() <= lsb, "channel {ch}: {volts} V reads back as {read:.5} V");
        }
    }
}
