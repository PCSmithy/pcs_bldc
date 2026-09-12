//! ADC conversion faults and dual-ADC multimode, asserted white-box against the real
//! firmware: the sim drivers' own statics are the injection point (DWARF write of the
//! DMA channel's stall knob) and the observation point (DWARF read of the ADC's
//! per-channel status and multimode flags), so the drivers carry no test-only API.

mod common;
use common::{assert_status, bool_at, booted, set_bool, u64_at, ADC_BUSY, ADC_FAULT};
use pcs_bldc_sil::Sil;

/// ADC peripherals the board configures — `HW_ADC_CHANNEL_COUNT`.
const CHANNELS: usize = 2;

/// The DMA channel each ADC's regular sequence transfers over —
/// `HW_DMA_CHANNEL_ADC1_REG_CONVERSIONS` / `_ADC2_` in the board config.
const DMA_CHANNEL: [usize; CHANNELS] = [2, 3];

/// A regular input enabled on ADC1 in the sim board config, undriven in a bare world
/// so it ramps once per sampling pass.
const RAMPING_INPUT: usize = 6;

/// One channel's most recent count on [`RAMPING_INPUT`].
fn count(sim: &Sil, ch: usize) -> u64 {
    u64_at(
        sim,
        &format!("HW_ADC_data.channelData[{ch}].counts[{RAMPING_INPUT}]"),
    )
}

/// Wedge or release one channel's DMA transfers: a transfer started while the
/// knob is set never completes.
fn set_stall(sim: &Sil, ch: usize, stall: bool) {
    set_bool(
        sim,
        &format!("HW_DMA_data.channels[{}].stall", DMA_CHANNEL[ch]),
        stall,
    );
}

/// One channel's conversion status path.
fn status_path(ch: usize) -> String {
    format!("HW_ADC_data.channelData[{ch}].status")
}

// [test->fw~hal_adc_009~1]
// [test->fw~hal_dma_004~1]
// At an exact-tick step boundary the DMA pass is deterministically in flight
// (the task sampled + started it; the completion dispatches at the next step's
// ISR phase), so a healthy channel observes BUSY here with counts valid from
// the previous completed pass. FAULT is the distinguishable failure signal.
// The wedged transfer is aborted by the pass that finds it, and its late
// completion, if any, is discarded.
#[test]
fn a_held_dma_completion_wedges_to_a_fault_and_recovers() {
    let mut sim = booted(2);
    assert_status(&sim, &status_path(0), &ADC_BUSY, "channel 0 healthy: pass in flight at the boundary");
    assert_status(&sim, &status_path(1), &ADC_BUSY, "channel 1 healthy: pass in flight at the boundary");
    assert_ne!(count(&sim, 0), 0, "boot passes have landed real counts");

    // Stalled: the pass in flight is the one wedged — its completion never
    // arrives, and the next pass records the overlap fault and aborts it.
    set_stall(&sim, 0, true);
    sim.run_for_ms(1);
    assert_status(
        &sim,
        &status_path(0),
        &ADC_FAULT,
        "a pass starting over the incomplete one records the fault",
    );
    let held = count(&sim, 0);

    // While the stall holds, every fresh start wedges and the pass after it
    // faults again; counts stay at the last completed pass's values; the
    // neighboring channel keeps cycling.
    sim.run_for_ms(2);
    assert_status(&sim, &status_path(0), &ADC_FAULT, "the fault holds while the stall does");
    assert_status(&sim, &status_path(1), &ADC_BUSY, "the neighboring channel is unaffected");
    assert_eq!(
        count(&sim, 0),
        held,
        "a faulted pass retains the last completed pass's count"
    );

    // Cleared: the next pass pends normally, its completion lands at the
    // following step, and fresh counts land. The synthetic ramp is driven by a
    // tick counter that runs through the stall, so the resumed value is fresh
    // but not `held + 1`.
    set_stall(&sim, 0, false);
    sim.run_for_ms(2);
    assert_status(&sim, &status_path(0), &ADC_BUSY, "recovered: back to the healthy in-flight cycle");
    assert_ne!(
        count(&sim, 0),
        held,
        "a recovered pass stores a fresh count"
    );
}

// [test->fw~hal_adc_007~1]
#[test]
fn multimode_is_applied_exactly_where_the_config_flags_a_master() {
    let sim = booted(1);

    let flagged: Vec<bool> = (0..CHANNELS)
        .map(|ch| bool_at(&sim, &format!("HW_ADC_channelConfig[{ch}].configureMultimode")))
        .collect();
    let applied: Vec<bool> = (0..CHANNELS)
        .map(|ch| bool_at(&sim, &format!("HW_ADC_data.channelData[{ch}].multimodeApplied")))
        .collect();

    assert_eq!(
        applied, flagged,
        "init applies multimode to the channels flagged master and to no others"
    );
    assert!(
        flagged.iter().any(|f| !f),
        "the board config leaves at least one channel unflagged, so 'and to no others' has a witness"
    );
}
