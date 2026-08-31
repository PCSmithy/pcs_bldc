//! SPI transfer faults, asserted white-box against the real firmware: the sim
//! driver's own statics are the injection point (DWARF write of the per-channel
//! stall / force-error flags) and the observation point (DWARF read of the
//! per-channel status), so the driver carries no test-only API. The SPI data
//! path itself is covered by the encoder/duplex suites.

use pcs_bldc_sil::{Sil, SOURCE};
use voyant::Value;

/// `HW_SPI_CHANNEL_AS5048_1` — a channel on the blocking (software) bus, driven
/// every ms by the encoder sampling pass.
const SW_CHANNEL: usize = 0;
/// `HW_SPI_CHANNEL_AS5048_2` — the software bus's other channel, the untouched
/// neighbor in the stall test.
const SW_NEIGHBOR: usize = 1;
/// `HW_SPI_CHANNEL_SK6805_STRING` — the channel on the non-blocking (DMA) bus,
/// driven every 10 ms by the LED frame transmit.
const NONBLOCKING_CHANNEL: usize = 2;

/// One `HW_SPI_status_E` value. Enumerator names do not resolve in this build
/// (see `docs/sil/backlog.md`), so a status matches by name OR by the DWARF
/// reader's `<ordinal>` placeholder.
struct Status {
    name: &'static str,
    ordinal: i64,
}

/// The status a channel reports after a successful transfer.
const COMPLETE: Status = Status {
    name: "HW_SPI_STATUS_COMPLETE",
    ordinal: 2,
};
/// The status a stalled or force-errored transfer leaves behind.
const ERROR: Status = Status {
    name: "HW_SPI_STATUS_ERROR",
    ordinal: 3,
};

/// A booted world with the firmware member added and enough time behind it for
/// both SPI consumers (1 ms encoder reads, 10 ms LED frames) to have transacted.
fn booted() -> Sil {
    let mut sim = Sil::new();
    let fwm = sim.load_firmware(SOURCE);
    sim.add_member(fwm);
    sim.run_for_ms(25);
    sim
}

/// Assert one channel's transfer status, read out of firmware memory.
fn assert_status(sim: &Sil, ch: usize, want: &Status, why: &str) {
    let path = format!("HW_SPI_data.channels[{ch}].status");
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

/// Set or clear a `bool` fault knob in the driver's channel data.
fn set_knob(sim: &Sil, ch: usize, knob: &str, value: bool) {
    sim.fw().write_cvar(
        &format!("HW_SPI_data.channels[{ch}].{knob}"),
        &Value::Bool(value),
    );
}

// [test->fw~hal_spi_003~1]
#[test]
fn a_stalled_software_transfer_errors_alone_and_recovers() {
    let mut sim = booted();
    assert_status(&sim, SW_CHANNEL, &COMPLETE, "encoder reads transact at boot");
    assert_status(&sim, SW_NEIGHBOR, &COMPLETE, "both encoder channels transact");

    // Stalled: the next blocking transfer times out and reports the error,
    // while the neighboring channel on the same bus keeps transacting.
    set_knob(&sim, SW_CHANNEL, "stall", true);
    sim.run_for_ms(2);
    assert_status(&sim, SW_CHANNEL, &ERROR, "a stalled transfer reports the timeout");
    assert_status(&sim, SW_NEIGHBOR, &COMPLETE, "the neighboring channel is unaffected");

    // Cleared: the next transfer goes through again.
    set_knob(&sim, SW_CHANNEL, "stall", false);
    sim.run_for_ms(2);
    assert_status(&sim, SW_CHANNEL, &COMPLETE, "clearing the stall resumes transfers");
}

// [test->fw~hal_spi_005~1]
#[test]
fn a_forced_error_fails_the_nonblocking_completion() {
    let mut sim = booted();
    // The engine dispatches the pended completion interrupt on its own: the LED
    // frame's DMA transfer has already walked BUSY -> COMPLETE.
    assert_status(
        &sim,
        NONBLOCKING_CHANNEL,
        &COMPLETE,
        "the LED frame completes through the pended interrupt",
    );

    // Forced: the next completion lands ERROR instead of COMPLETE.
    set_knob(&sim, NONBLOCKING_CHANNEL, "forceError", true);
    sim.run_for_ms(20);
    assert_status(
        &sim,
        NONBLOCKING_CHANNEL,
        &ERROR,
        "a forced error fails the completion",
    );

    // Cleared: the next frame completes cleanly again.
    set_knob(&sim, NONBLOCKING_CHANNEL, "forceError", false);
    sim.run_for_ms(20);
    assert_status(
        &sim,
        NONBLOCKING_CHANNEL,
        &COMPLETE,
        "clearing the flag restores clean completions",
    );
}
