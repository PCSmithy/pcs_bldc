//! DMA completion faults, asserted white-box against the real firmware: the sim
//! driver's own statics are the injection point (DWARF writes fabricate an
//! in-flight transfer and set the per-channel force-error knob) and the
//! observation point (status + transfer counter), and the completion interrupt
//! is dispatched by name on the engine grid — the driver carries no test-only API.
//!
//! Fabrication is the only route in: the sim `HW_SPI` models its own non-blocking
//! completion rather than delegating to `HW_DMA_startTransfer` (only the stm32g4
//! SPI driver does that), so no firmware path in this world starts a transfer.
//! `HW_DMA_startTransfer` itself — argument validation, the direction dispatch,
//! the mem->periph `lastMem` capture — is unit-tested in `test_HW_DMA.c`.

mod common;
use common::{assert_status, set_bool, u64_at, Status, DMA_COMPLETE, DMA_ERROR, DMA_IDLE};
use pcs_bldc_sil::{Sil, SOURCE};

/// The completion handler the sim driver registers pended at `HW_DMA_init`.
const DMA_ISR: &str = "HW_DMA_sim_completionDispatch";

/// `HW_DMA_CHANNEL_SK6805_TX` — memory-to-peripheral, so a fabricated completion
/// never dereferences the transfer's (unset) memory pointer.
const TX: usize = 0;

fn assert_channel(sim: &Sil, ch: usize, want: &Status, why: &str) {
    assert_status(sim, &format!("HW_DMA_data.channels[{ch}].status"), want, why);
}

/// One channel's completed-transfer count.
fn transfer_count(sim: &Sil, ch: usize) -> u64 {
    u64_at(sim, &format!("HW_DMA_data.channels[{ch}].transferCount"))
}

/// Write one `bool` field of the channel's data.
fn set_flag(sim: &Sil, ch: usize, field: &str, v: bool) {
    set_bool(sim, &format!("HW_DMA_data.channels[{ch}].{field}"), v);
}

// [test->fw~hal_dma_003~1]
#[test]
fn a_forced_error_lands_error_status_and_the_next_completion_recovers() {
    let mut sim = Sil::new();
    let fwm = sim.load_firmware(SOURCE);
    let member = sim.add_member(fwm);
    sim.run_for_ms(1);

    // The driver wired its completion service into the engine at init.
    member
        .borrow()
        .find_isr(DMA_ISR)
        .expect("HW_DMA_init registered its completion interrupt through the SIL_irq upcall");

    assert_channel(&sim, TX, &DMA_IDLE, "no transfer has run at boot");
    assert_eq!(transfer_count(&sim, TX), 0, "no completions at boot");

    // Fabricate an in-flight transfer with the fault knob set, then run the
    // completion interrupt: the transfer settles ERROR and is consumed.
    set_flag(&sim, TX, "pending", true);
    set_flag(&sim, TX, "forceError", true);
    member
        .borrow_mut()
        .register_oneshot_isr(DMA_ISR, 0, 8)
        .expect("the completion handler resolves by name");
    sim.run_for_ms(2);
    assert_channel(&sim, TX, &DMA_ERROR, "a force-errored completion reports the fault");
    assert_eq!(transfer_count(&sim, TX), 1, "the errored transfer still counts");

    // Knob cleared: the next fabricated transfer completes clean — the error
    // does not latch past its own transfer.
    set_flag(&sim, TX, "forceError", false);
    set_flag(&sim, TX, "pending", true);
    member
        .borrow_mut()
        .register_oneshot_isr(DMA_ISR, 0, 8)
        .expect("the completion handler resolves by name");
    sim.run_for_ms(2);
    assert_channel(&sim, TX, &DMA_COMPLETE, "clearing the knob restores clean completion");
    assert_eq!(transfer_count(&sim, TX), 2, "each completion counts exactly once");
}
