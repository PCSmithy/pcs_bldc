//! The opt-in sub-millisecond engine grid and the gated cvar mirror that pays for it.
//!
//! A world built on a fine grid runs the real firmware unchanged: the kernel tick keeps
//! its own millisecond cadence while a sub-millisecond interrupt dispatches on the steps
//! between ticks. The mirror sweep then runs on a cadence of its own, which delays a
//! firmware write into the historian by a bounded amount and never loses it; a scenario
//! that needs the current value now forces a sweep.

use pcs_bldc_sil::board::{board_with, fault_latched, gate_operational, GATE_BRINGUP_MS};
use pcs_bldc_sil::{cid, cvar, Board, Sil, SilOptions, SOURCE};
use std::cell::RefCell;
use std::rc::Rc;
use voyant::FirmwareMember;

/// The PWM period at 20 kHz — the grid a center-aligned control interrupt needs.
const FINE_GRID_US: u64 = 50;
/// Grid steps in one kernel tick period.
const STEPS_PER_TICK: u64 = 1_000 / FINE_GRID_US;

/// A sub-millisecond interrupt period, an exact number of grid steps.
const FAST_PERIOD_US: u64 = 250;
/// Grid steps between two of its dispatches.
const STEPS_PER_FAST: u64 = FAST_PERIOD_US / FINE_GRID_US;

/// The handler the sim USB driver registers by pointer at `HW_USB_init`; it notifies
/// `task_usb`, so `taskUsbRuns` counts its dispatches.
const USB_ISR: &str = "HW_USB_sim_irqHandler";

/// One firmware static read straight out of memory, past the historian.
fn mem(sim: &Sil, path: &str) -> u64 {
    sim.fw().read_cvar(path).as_u64().unwrap_or(0)
}

/// A fine-grid world with a `FAST_PERIOD_US` interrupt registered on the USB handler
/// and the driver's own 1 ms entry masked, so that handler runs on exactly one cadence.
/// Returns the world and its firmware member.
fn fine_world(options: SilOptions) -> (Sil, Rc<RefCell<FirmwareMember>>) {
    let mut sim = options.grid_us(FINE_GRID_US).build();
    let mut fwm = sim.load_firmware(SOURCE);
    let fast = fwm
        .register_periodic_isr(USB_ISR, FAST_PERIOD_US, 0)
        .expect("the handler name resolves to a function in the image");
    let member = sim.add_member(fwm);
    let driver = member
        .borrow()
        .find_isr(USB_ISR)
        .expect("HW_USB_init registered its own entry on this handler");
    assert_ne!(driver, fast, "two entries share this handler address");
    member.borrow_mut().set_isr_enabled(driver, false);
    (sim, member)
}

#[test]
fn a_sub_millisecond_interrupt_runs_between_kernel_ticks() {
    // The kernel tick is scheduled in sim time, so refining the grid leaves its
    // millisecond alone and only adds steps in between — which is where the faster
    // interrupt lands.
    let (mut sim, member) = fine_world(Sil::options());
    let boot = mem(&sim, "taskUsbRuns");

    for step in 1..=(5 * STEPS_PER_TICK) {
        sim.step().expect("engine step");
        assert_eq!(
            mem(&sim, "xTickCount"),
            step / STEPS_PER_TICK,
            "step {step}: the kernel tick holds its 1 ms cadence"
        );
        assert_eq!(
            mem(&sim, "taskUsbRuns"),
            boot + (step / STEPS_PER_FAST),
            "step {step}: the faster interrupt dispatches on its own cadence"
        );
    }
    // 20 dispatches of the fast entry, 5 kernel ticks.
    assert_eq!(member.borrow().isr_dispatch_count(), 25);
}

#[test]
fn the_gated_mirror_delays_a_cvar_change_by_at_most_the_cadence() {
    // The kernel tick counter advances once per millisecond of sim time, so under a
    // 1 ms mirror cadence the historian may trail firmware memory by one count — and
    // by no more, and without ever skipping one.
    let (mut sim, _member) = fine_world(Sil::options());
    assert_eq!(sim.tick_period_us(), FINE_GRID_US);

    let mut lagged = 0u64;
    for step in 1..=(5 * STEPS_PER_TICK) {
        sim.step().expect("engine step");
        let memory = mem(&sim, "xTickCount");
        let historian = sim.read_u64(&cid("xTickCount"));
        assert!(
            (historian <= memory) && ((memory - historian) <= 1),
            "step {step}: the mirror trails memory by at most the cadence, got \
             {historian} against {memory}"
        );
        lagged += u64::from(historian < memory);
    }
    assert!(lagged > 0, "the gate actually withheld sweeps");

    // Every value the counter took is in the change log: the sweep delays a record,
    // it never drops one. The last one is still in flight at the end of the run, so
    // collect it the way an assert would.
    sim.mirror_now();
    let logged: Vec<u64> = sim
        .state()
        .changes(&cvar("xTickCount"))
        .expect("xTickCount is mirrored")
        .into_iter()
        .filter_map(|(_, v)| v.as_u64())
        .collect();
    assert_eq!(logged, vec![0, 1, 2, 3, 4, 5]);
}

#[test]
fn an_ungated_mirror_keeps_the_historian_current_every_step() {
    // The gate is the whole difference: with the cadence off, the same run mirrors on
    // every dispatching step and the historian never trails.
    let (mut sim, _member) = fine_world(Sil::options().sweep_period_us(0));
    for step in 1..=(2 * STEPS_PER_TICK) {
        sim.step().expect("engine step");
        assert_eq!(
            sim.read_u64(&cid("xTickCount")),
            mem(&sim, "xTickCount"),
            "step {step}: an ungated mirror is current"
        );
    }
}

#[test]
fn a_forced_mirror_makes_the_historian_current_for_an_assert() {
    // The assert path: a scenario that needs the value now asks for a sweep instead of
    // waiting for the cadence.
    let (mut sim, _member) = fine_world(Sil::options());
    for _ in 0..STEPS_PER_TICK {
        sim.step().expect("engine step");
    }
    let memory = mem(&sim, "xTickCount");
    assert_eq!(memory, 1, "one kernel tick of sim time has elapsed");
    assert_ne!(
        sim.read_u64(&cid("xTickCount")),
        memory,
        "the cadence has not swept since the tick landed"
    );

    sim.mirror_now();
    assert_eq!(sim.read_u64(&cid("xTickCount")), memory);
    assert_eq!(sim.now_us(), 1_000, "forcing a mirror advances no sim time");
}

#[test]
fn the_board_world_is_built_from_the_same_options() {
    // The board world is built from the same options as any other, so a scenario picks
    // its grid and mirror cadence at the call site rather than in board.rs. Here: the
    // mirror gate off, which must leave the board behaving as it always has.
    let Board { mut sim, .. } = board_with(Sil::options().sweep_period_us(0), 0.8);
    sim.run_for_ms(GATE_BRINGUP_MS);
    assert!(gate_operational(&sim), "gate driver operational after boot");
    assert!(!fault_latched(&sim), "no fault latched through boot");
}
