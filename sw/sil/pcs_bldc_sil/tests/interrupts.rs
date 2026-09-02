//! The framework interrupt controller against the real firmware image
//! (`docs/sil/sim-interrupts.md`): both registration paths reach the firmware, the
//! handler runs inside the port's ISR bracket, and its `...FromISR` wakeup runs a
//! real FreeRTOS task before the step returns to quiescence.
//!
//! The witness is the sim `HW_USB` driver, which registers its simulated device
//! interrupt at init and blocks `task_usb` on a task notification the handler gives —
//! the same shape as the real USB ISR waking the service task. `taskUsbRuns` is that
//! task's heartbeat, so the counter moving in a step is proof the woken task ran.

mod common;
use common::SYSTICK_ISR;
use pcs_bldc_sil::{cid, Sil, SOURCE};

/// The handler the sim USB driver registers by pointer at `HW_USB_init`.
const USB_ISR: &str = "HW_USB_sim_irqHandler";
/// The sim ADC's completion service, pended once per step in a booted image.
const ADC_ISR: &str = "HW_ADC_sim_completionDispatch";

#[test]
fn a_driver_registered_interrupt_wakes_a_real_task_every_step() {
    let mut sim = Sil::new();
    let fwm = sim.load_firmware(SOURCE);
    // Boot leaves task_usb having run once, then blocked on its notification.
    let boot = sim.fw().read_cvar("taskUsbRuns").as_u64().unwrap_or(0);
    assert_eq!(boot, 1, "task_usb runs once at boot, then waits for its interrupt");

    let member = sim.add_member(fwm);
    // The registration the C driver made during sil_fw_start is live from enable.
    let usb_irq = member
        .borrow()
        .find_isr(USB_ISR)
        .expect("HW_USB_init registered its interrupt through the SIL_irq upcall");

    // One USB dispatch per 1 ms step, and the task it wakes runs INSIDE that step: the
    // counter read back from the historian has already advanced when step() returns.
    for n in 1..=10u64 {
        sim.step().expect("engine step");
        assert_eq!(
            sim.read_u64(&cid("taskUsbRuns")),
            boot + n,
            "step {n}: the woken task must run before the step returns to quiescence"
        );
    }
    assert_eq!(member.borrow().isr_dispatch_count_of(usb_irq), 10);

    // Per-IRQ enable: masking the entry stops the wakeups dead — and only those; the
    // kernel tick beside it keeps dispatching.
    member.borrow_mut().set_isr_enabled(usb_irq, false);
    for _ in 0..5 {
        sim.step().expect("engine step");
    }
    assert_eq!(
        sim.read_u64(&cid("taskUsbRuns")),
        boot + 10,
        "a disabled interrupt dispatches nothing, so the task never wakes"
    );
    assert_eq!(member.borrow().isr_dispatch_count_of(usb_irq), 10);

    // Re-enabling resumes the cadence — nothing was lost but the masked window.
    member.borrow_mut().set_isr_enabled(usb_irq, true);
    for n in 1..=5u64 {
        sim.step().expect("engine step");
        assert_eq!(sim.read_u64(&cid("taskUsbRuns")), boot + 10 + n);
    }
}

#[test]
fn a_config_time_one_shot_resolves_by_name_and_fires_on_its_grid_step() {
    let mut sim = Sil::new();
    let mut fwm = sim.load_firmware(SOURCE);

    // Config-time path: the scenario names the handler and the framework resolves it
    // through the image's DWARF. 2500 us from t=0 is due mid-step, so it quantizes up
    // to the 3 ms grid step.
    let one_shot = fwm
        .register_oneshot_isr(USB_ISR, 2_500, 0)
        .expect("the handler name resolves to a function in the image");
    assert!(
        fwm.register_oneshot_isr("NoSuchIrqHandler", 1_000, 0).is_none(),
        "an unknown name registers nothing"
    );

    let member = sim.add_member(fwm);
    // Silence the driver's own periodic (registered first, so find_isr returns it) so
    // the one-shot is the ONLY thing that can wake the task.
    let periodic = member.borrow().find_isr(USB_ISR).expect("the driver's periodic");
    assert_ne!(periodic, one_shot, "two entries share this handler address");
    member.borrow_mut().set_isr_enabled(periodic, false);

    // From firmware memory: nothing has been mirrored into the historian yet.
    let boot = sim.fw().read_cvar("taskUsbRuns").as_u64().unwrap_or(0);
    // Steps at 1 and 2 ms: not yet due. At 3 ms the one-shot lands and wakes the task.
    // Steps 4..6: a one-shot fires once and is gone.
    for step in 1..=6u64 {
        sim.step().expect("engine step");
        let expected = boot + u64::from(step >= 3);
        assert_eq!(
            sim.read_u64(&cid("taskUsbRuns")),
            expected,
            "step {step} ({} us)",
            step * 1_000
        );
    }
    // The traffic canary, stated as a decomposition: each live source on its own,
    // then the world total against their sum plus the one-shot — whose entry is
    // pruned when it fires, so the loop above is its only witness. An interrupt
    // source appearing unannounced breaks the sum, and a moved term names itself.
    let m = member.borrow();
    let ticks = m.find_isr(SYSTICK_ISR).expect("the port's kernel tick");
    let adc = m.find_isr(ADC_ISR).expect("the ADC completion service");
    assert_eq!(m.isr_dispatch_count_of(ticks), 6, "one kernel tick per step");
    assert_eq!(m.isr_dispatch_count_of(adc), 6, "one ADC completion per step");
    assert_eq!(
        m.isr_dispatch_count(),
        m.isr_dispatch_count_of(ticks) + m.isr_dispatch_count_of(adc) + 1,
        "an unaccounted-for interrupt source appeared in the world"
    );
}

#[test]
fn the_kernel_tick_is_a_table_entry_like_any_other() {
    // The port's systick registers through the same upcall as any sim driver, so
    // masking it stops the kernel clock while the USB interrupt keeps running.
    let mut sim = Sil::new();
    let fwm = sim.load_firmware(SOURCE);
    let member = sim.add_member(fwm);
    let systick = member
        .borrow()
        .find_isr(SYSTICK_ISR)
        .expect("the port registered its kernel tick at scheduler start");

    for n in 1..=5u64 {
        sim.step().expect("engine step");
        assert_eq!(sim.read_u64(&cid("xTickCount")), n, "one kernel tick per step");
    }
    let usb_runs = sim.read_u64(&cid("taskUsbRuns"));

    member.borrow_mut().set_isr_enabled(systick, false);
    for _ in 0..5 {
        sim.step().expect("engine step");
    }
    assert_eq!(sim.read_u64(&cid("xTickCount")), 5, "a masked tick freezes the kernel");
    assert_eq!(
        sim.read_u64(&cid("taskUsbRuns")),
        usb_runs + 5,
        "the USB interrupt is unaffected"
    );
}
