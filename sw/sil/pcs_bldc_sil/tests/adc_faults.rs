//! ADC conversion faults and dual-ADC multimode, asserted white-box against the real
//! firmware: the sim driver's own statics are the injection point (DWARF write of the
//! per-channel stall flag) and the observation point (DWARF read of the per-channel
//! status and multimode flags), so the driver carries no test-only API.

mod common;
use common::{assert_status, bool_at, booted, set_bool, u64_at, ADC_FAULT, ADC_OK};
use pcs_bldc_sil::Sil;

/// ADC peripherals the board configures — `HW_ADC_CHANNEL_COUNT`.
const CHANNELS: usize = 2;

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

/// Stall or un-stall one channel's conversions.
fn set_stall(sim: &Sil, ch: usize, stall: bool) {
    set_bool(sim, &format!("HW_ADC_data.conversionStall[{ch}]"), stall);
}

/// One channel's conversion status path.
fn status_path(ch: usize) -> String {
    format!("HW_ADC_data.status[{ch}]")
}

// [test->fw~hal_adc_004~1]
#[test]
fn a_stalled_channel_faults_alone_and_recovers() {
    let mut sim = booted(1);
    assert_status(&sim, &status_path(0), &ADC_OK, "channel 0 samples cleanly at boot");
    assert_status(&sim, &status_path(1), &ADC_OK, "channel 1 samples cleanly at boot");
    let before = count(&sim, 0);

    // Stalled: the pass faults and leaves the counts where they were, while the
    // neighboring channel keeps sampling.
    set_stall(&sim, 0, true);
    sim.run_for_ms(1);
    assert_status(&sim, &status_path(0), &ADC_FAULT, "a stalled channel reports the fault");
    assert_status(&sim, &status_path(1), &ADC_OK, "the neighboring channel is unaffected");
    assert_eq!(
        count(&sim, 0),
        before,
        "a faulted pass retains the prior count"
    );

    // A second stalled pass holds the fault rather than aging out of it.
    sim.run_for_ms(1);
    assert_status(&sim, &status_path(0), &ADC_FAULT, "the fault holds while the stall does");
    assert_eq!(
        count(&sim, 0),
        before,
        "counts stay stale for as long as the stall"
    );

    // Cleared: sampling resumes and the counts move again. The synthetic ramp is
    // driven by a tick counter that runs through the stall, so the resumed value
    // is fresh but not `before + 1`.
    set_stall(&sim, 0, false);
    sim.run_for_ms(1);
    assert_status(&sim, &status_path(0), &ADC_OK, "clearing the stall resumes sampling");
    assert_ne!(
        count(&sim, 0),
        before,
        "a resumed pass stores a fresh count"
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
        .map(|ch| bool_at(&sim, &format!("HW_ADC_data.multimodeApplied[{ch}]")))
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
