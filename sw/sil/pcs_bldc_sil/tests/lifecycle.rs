//! World lifecycle: the firmware DLL reloads from reset every world, and the
//! fiber port re-initializes cleanly on a fresh thread.
//!
//! This is the harness gate. The fiber port converts its thread to a Windows fiber
//! at `start()` and never converts back, so a second `start()` on the *same* thread
//! aborts — which is exactly why each `#[test]` runs on its own freshly-spawned
//! thread and constructs exactly one world. These tests prove the reload cycle: a
//! dropped world unloads the library (last `Rc` gone), and the next load boots C
//! statics from scratch.

use pcs_bldc_sil::{dll_path, lock_world, Sil};
use voyant::Firmware;

/// Drive one raw load → start → tick → read → shutdown cycle, asserting the
/// firmware booted from reset (counters at 0, clock at 0) and its clock advances
/// exactly one tick (1000 µs) per `advance_tick`.
fn reset_cycle(cycle: usize) {
    let fw = Firmware::load(&dll_path()).expect("load firmware");
    assert!(fw.start(), "cycle {cycle}: sil_fw_start");

    let runs_before = fw.read_cvar("task1msRuns").as_u64().unwrap();
    let time_before = fw.read_cvar("lib_timer_data.currentTime_us").as_u64().unwrap();
    assert_eq!(runs_before, 0, "cycle {cycle}: task1msRuns must be 0 from reset");
    assert_eq!(time_before, 0, "cycle {cycle}: firmware clock must start at 0 from reset");

    const K: u64 = 5;
    for _ in 0..K {
        fw.advance_tick();
    }
    let time_after = fw.read_cvar("lib_timer_data.currentTime_us").as_u64().unwrap();
    let runs_after = fw.read_cvar("task1msRuns").as_u64().unwrap();
    assert_eq!(time_after, K * 1000, "cycle {cycle}: +1000 us/tick while alive");
    assert_eq!(runs_after, K, "cycle {cycle}: task_1ms fires once per tick");

    fw.shutdown();
    // Dropping `fw` here unloads the DLL; the next cycle's load boots from reset.
}

#[test]
fn reload_cycles_reset_on_fresh_threads() {
    // Hold the world lock so no concurrent Sil-based test loads the DLL while this
    // spike drives its own raw reload cycles (the DLL image and its C statics are
    // process-global; only one live load is safe).
    let _guard = lock_world();
    // Each cycle on its own freshly-spawned thread, joined before the next — proving
    // the fiber port re-initializes on a thread that was never converted before, and
    // that only one DLL is ever live at a time.
    for cycle in 0..4 {
        std::thread::spawn(move || reset_cycle(cycle))
            .join()
            .expect("reload cycle thread");
    }
}

#[test]
fn boot_is_fresh() {
    // A single fresh world (this test's own cargo-test thread): the firmware boots
    // from reset and its clock climbs exactly one tick per engine step.
    let mut world = Sil::new();
    let fwm = world.firmware_member();
    world.sim.add_member(fwm);

    assert_eq!(
        world.fw().read_cvar("task1msRuns").as_u64().unwrap(),
        0,
        "task1msRuns is 0 before the first step"
    );
    assert_eq!(
        world.fw().read_cvar("lib_timer_data.currentTime_us").as_u64().unwrap(),
        0,
        "firmware clock starts at 0 from reset"
    );

    for k in 1..=5u64 {
        world.sim.step().expect("engine step");
        let time = world
            .fw()
            .read_cvar("lib_timer_data.currentTime_us")
            .as_u64()
            .unwrap();
        assert_eq!(time, 1000 * k, "firmware clock is +1000 us/tick (tick {k})");
    }
}
