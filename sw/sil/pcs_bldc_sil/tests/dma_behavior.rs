//! DMA completion faults, asserted white-box against the real firmware: the sim
//! driver's own statics are the injection point (DWARF writes fabricate an
//! in-flight transfer and set the per-channel force-error knob) and the
//! observation point (status + transfer counter), and the completion interrupt
//! is dispatched by name on the engine grid — the driver carries no test-only API.

use pcs_bldc_sil::{Sil, SOURCE};
use voyant::Value;

/// The completion handler the sim driver registers pended at `HW_DMA_init`.
const DMA_ISR: &str = "HW_DMA_sim_completionDispatch";

/// `HW_DMA_CHANNEL_SK6805_TX` — memory-to-peripheral, so a fabricated completion
/// never dereferences the transfer's (unset) memory pointer.
const TX: usize = 0;

/// One `HW_DMA_status_E` value. Enumerator names do not resolve in this build
/// (see `docs/sil/backlog.md`), so a status matches by name OR by the DWARF
/// reader's `<ordinal>` placeholder.
struct Status {
    name: &'static str,
    ordinal: i64,
}

/// The status of a channel with no transfer since init.
const IDLE: Status = Status {
    name: "HW_DMA_STATUS_IDLE",
    ordinal: 0,
};
/// The status a successful completion lands.
const COMPLETE: Status = Status {
    name: "HW_DMA_STATUS_COMPLETE",
    ordinal: 2,
};
/// The status a force-errored completion lands.
const ERROR: Status = Status {
    name: "HW_DMA_STATUS_ERROR",
    ordinal: 3,
};

/// Assert one channel's transfer status, read out of firmware memory.
fn assert_status(sim: &Sil, ch: usize, want: &Status, why: &str) {
    let path = format!("HW_DMA_data.channels[{ch}].status");
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

/// One channel's completed-transfer count.
fn transfer_count(sim: &Sil, ch: usize) -> u64 {
    let path = format!("HW_DMA_data.channels[{ch}].transferCount");
    sim.fw()
        .read_cvar(&path)
        .as_u64()
        .unwrap_or_else(|| panic!("{path} reads as an unsigned count"))
}

/// Write one `bool` field of the channel's data.
fn set_flag(sim: &Sil, ch: usize, field: &str, v: bool) {
    sim.fw().write_cvar(
        &format!("HW_DMA_data.channels[{ch}].{field}"),
        &Value::Bool(v),
    );
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

    assert_status(&sim, TX, &IDLE, "no transfer has run at boot");
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
    assert_status(&sim, TX, &ERROR, "a force-errored completion reports the fault");
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
    assert_status(&sim, TX, &COMPLETE, "clearing the knob restores clean completion");
    assert_eq!(transfer_count(&sim, TX), 2, "each completion counts exactly once");
}
