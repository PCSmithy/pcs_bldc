//! SPI transfer faults, asserted white-box against the real firmware: the sim
//! driver's own statics are the injection point (DWARF write of the per-channel
//! stall / force-error flags) and the observation point (DWARF read of the
//! per-channel status), so the driver carries no test-only API. The SPI data
//! path itself is covered by the encoder/duplex suites.

mod common;
use common::{assert_status, booted, set_bool, Status, SPI_COMPLETE, SPI_ERROR};
use pcs_bldc_sil::Sil;

/// `HW_SPI_CHANNEL_AS5048_1` — a channel on the blocking (software) bus, driven
/// every ms by the encoder sampling pass.
const SW_CHANNEL: usize = 0;
/// `HW_SPI_CHANNEL_AS5048_2` — the software bus's other channel, the untouched
/// neighbor in the stall test.
const SW_NEIGHBOR: usize = 1;
/// `HW_SPI_CHANNEL_SK6805_STRING` — the channel on the non-blocking (DMA) bus,
/// driven every 10 ms by the LED frame transmit.
const NONBLOCKING_CHANNEL: usize = 2;

/// Enough time for both SPI consumers (1 ms encoder reads, 10 ms LED frames) to
/// have transacted.
const BOOT_MS: u64 = 25;

fn assert_channel(sim: &Sil, ch: usize, want: &Status, why: &str) {
    assert_status(sim, &format!("HW_SPI_data.channels[{ch}].status"), want, why);
}

/// Set or clear a `bool` fault knob in the driver's channel data.
fn set_knob(sim: &Sil, ch: usize, knob: &str, value: bool) {
    set_bool(sim, &format!("HW_SPI_data.channels[{ch}].{knob}"), value);
}

// [test->fw~hal_spi_003~1]
#[test]
fn a_stalled_software_transfer_errors_alone_and_recovers() {
    let mut sim = booted(BOOT_MS);
    assert_channel(&sim, SW_CHANNEL, &SPI_COMPLETE, "encoder reads transact at boot");
    assert_channel(&sim, SW_NEIGHBOR, &SPI_COMPLETE, "both encoder channels transact");

    // Stalled: the next blocking transfer times out and reports the error,
    // while the neighboring channel on the same bus keeps transacting.
    set_knob(&sim, SW_CHANNEL, "stall", true);
    sim.run_for_ms(2);
    assert_channel(&sim, SW_CHANNEL, &SPI_ERROR, "a stalled transfer reports the timeout");
    assert_channel(&sim, SW_NEIGHBOR, &SPI_COMPLETE, "the neighboring channel is unaffected");

    // Cleared: the next transfer goes through again.
    set_knob(&sim, SW_CHANNEL, "stall", false);
    sim.run_for_ms(2);
    assert_channel(&sim, SW_CHANNEL, &SPI_COMPLETE, "clearing the stall resumes transfers");
}

// [test->fw~hal_spi_005~1]
#[test]
fn a_forced_error_fails_the_nonblocking_completion() {
    let mut sim = booted(BOOT_MS);
    // The engine dispatches the pended completion interrupt on its own: the LED
    // frame's DMA transfer has already walked BUSY -> COMPLETE.
    assert_channel(
        &sim,
        NONBLOCKING_CHANNEL,
        &SPI_COMPLETE,
        "the LED frame completes through the pended interrupt",
    );

    set_knob(&sim, NONBLOCKING_CHANNEL, "forceError", true);
    sim.run_for_ms(20);
    assert_channel(
        &sim,
        NONBLOCKING_CHANNEL,
        &SPI_ERROR,
        "a forced error fails the completion",
    );

    set_knob(&sim, NONBLOCKING_CHANNEL, "forceError", false);
    sim.run_for_ms(20);
    assert_channel(
        &sim,
        NONBLOCKING_CHANNEL,
        &SPI_COMPLETE,
        "clearing the flag restores clean completions",
    );
}
