//! ADC conversion faults and dual-ADC multimode, asserted white-box against the real
//! firmware: the sim driver's own statics are the injection point (DWARF write of the
//! per-channel stall flag) and the observation point (DWARF read of the per-channel
//! status and multimode flags), so the driver carries no test-only API.

use pcs_bldc_sil::{Sil, SOURCE};
use voyant::Value;

/// ADC peripherals the board configures — `HW_ADC_CHANNEL_COUNT`.
const CHANNELS: usize = 2;

/// A regular input enabled on ADC1 in the sim board config, undriven in a bare world
/// so it ramps once per sampling pass.
const RAMPING_INPUT: usize = 6;

/// One `HW_ADC_conversionStatus_E` value. Enumerator names do not resolve in this
/// build (see `docs/sil/backlog.md`), so a status matches by name OR by the DWARF
/// reader's `<ordinal>` placeholder.
struct Status {
    name: &'static str,
    ordinal: i64,
}

/// The status a channel reports after a pass that stored every enabled input.
const OK: Status = Status {
    name: "HW_ADC_CONVERSION_STATUS_OK",
    ordinal: 2,
};
/// The status a stalled channel reports — the sim stand-in for a poll timeout.
const FAULT: Status = Status {
    name: "HW_ADC_CONVERSION_STATUS_FAULT",
    ordinal: 3,
};

/// A booted world with the firmware member added and one sampling pass behind it.
fn booted() -> Sil {
    let mut sim = Sil::new();
    let fwm = sim.load_firmware(SOURCE);
    sim.add_member(fwm);
    sim.run_for_ms(1);
    sim
}

/// Assert one channel's conversion status, read out of firmware memory past the
/// historian.
fn assert_status(sim: &Sil, ch: usize, want: &Status, why: &str) {
    let path = format!("HW_ADC_data.status[{ch}]");
    let got = match sim.fw().read_cvar(&path) {
        Value::Enum(name) => name,
        other => panic!("{path} reads as an enum, got {other:?}"),
    };
    assert!(
        (got == want.name) || (got == format!("<{}>", want.ordinal)),
        "{why}: {path} is {got}, expected {}",
        want.name
    );
}

/// One channel's most recent count on [`RAMPING_INPUT`].
fn count(sim: &Sil, ch: usize) -> u64 {
    let path = format!("HW_ADC_data.channelData[{ch}].counts[{RAMPING_INPUT}]");
    sim.fw()
        .read_cvar(&path)
        .as_u64()
        .unwrap_or_else(|| panic!("{path} reads as an unsigned count"))
}

/// A `bool` firmware field.
fn flag(sim: &Sil, path: &str) -> bool {
    match sim.fw().read_cvar(path) {
        Value::Bool(b) => b,
        other => panic!("{path} reads as a bool, got {other:?}"),
    }
}

/// Stall or un-stall one channel's conversions.
fn set_stall(sim: &Sil, ch: usize, stall: bool) {
    sim.fw().write_cvar(
        &format!("HW_ADC_data.conversionStall[{ch}]"),
        &Value::Bool(stall),
    );
}

// [test->fw~hal_adc_004~1]
#[test]
fn a_stalled_channel_faults_alone_and_recovers() {
    let mut sim = booted();
    assert_status(&sim, 0, &OK, "channel 0 samples cleanly at boot");
    assert_status(&sim, 1, &OK, "channel 1 samples cleanly at boot");
    let before = count(&sim, 0);

    // Stalled: the pass faults and leaves the counts where they were, while the
    // neighboring channel keeps sampling.
    set_stall(&sim, 0, true);
    sim.run_for_ms(1);
    assert_status(&sim, 0, &FAULT, "a stalled channel reports the fault");
    assert_status(&sim, 1, &OK, "the neighboring channel is unaffected");
    assert_eq!(
        count(&sim, 0),
        before,
        "a faulted pass retains the prior count"
    );

    // A second stalled pass holds the fault rather than aging out of it.
    sim.run_for_ms(1);
    assert_status(&sim, 0, &FAULT, "the fault holds while the stall does");
    assert_eq!(
        count(&sim, 0),
        before,
        "counts stay stale for as long as the stall"
    );

    // Cleared: sampling resumes and the counts move again.
    set_stall(&sim, 0, false);
    sim.run_for_ms(1);
    assert_status(&sim, 0, &OK, "clearing the stall resumes sampling");
    assert_ne!(
        count(&sim, 0),
        before,
        "a resumed pass stores a fresh count"
    );
}

// [test->fw~hal_adc_007~1]
#[test]
fn multimode_is_applied_exactly_where_the_config_flags_a_master() {
    let sim = booted();

    let flagged: Vec<bool> = (0..CHANNELS)
        .map(|ch| {
            flag(
                &sim,
                &format!("HW_ADC_channelConfig[{ch}].configureMultimode"),
            )
        })
        .collect();
    let applied: Vec<bool> = (0..CHANNELS)
        .map(|ch| flag(&sim, &format!("HW_ADC_data.multimodeApplied[{ch}]")))
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
