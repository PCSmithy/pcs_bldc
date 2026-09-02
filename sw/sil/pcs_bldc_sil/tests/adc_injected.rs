//! Timer-triggered injected sampling: the sim twin of the TIM1 TRGO2 → ADC1/ADC2
//! chain, asserted white-box against the real firmware. The sim board config arms
//! both channels (rising edge, one slot each on pins IN6/IN7); triggers land during
//! the TIM advance and completions drain through the SIL_irq service, so status,
//! counts, and cadence are all observable from firmware memory.

mod common;
use common::{assert_status, booted, u64_at, ADC_OK};
use pcs_bldc_sil::{vid, Sil, SOURCE};

const U_PIN_PORT: &str = "ADC1_IN6";
const V_PIN_PORT: &str = "ADC2_IN7";

/// Quantize a driven pin voltage the way the 12-bit converter model does.
fn counts_of(volts: f64) -> u64 {
    ((volts / 3.3) * 4095.0 + 0.5) as u64
}

fn injected_count(sim: &Sil, ch: usize) -> u64 {
    u64_at(sim, &format!("HW_ADC_data.channelData[{ch}].injectedCounts[0]"))
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
            &format!("HW_ADC_data.injectedStatus[{ch}]"),
            &ADC_OK,
            &format!("channel {ch} injected conversion completed"),
        );
    }
    assert_eq!(
        u64_at(&sim, "HW_ADC_data.pendingCompletions[0]"),
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

    assert_eq!(injected_count(&sim, 0), counts_of(U_VOLTS), "U slot quantizes the pin");
    assert_eq!(injected_count(&sim, 1), counts_of(V_VOLTS), "V slot quantizes the pin");
    assert_eq!(
        u64_at(&sim, "HW_ADC_data.channelData[0].counts[6]"),
        counts_of(U_VOLTS),
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
            counts_of(*volts),
            "period {i}: this period's trigger captures the new pin voltage"
        );
    }
}
