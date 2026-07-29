//! All four real FreeRTOS tasks advance under the sim scheduler, and the sim
//! timebase (lib_timer, fed by TIM2) flows with sim time.

use pcs_bldc_sil::{cid, cvar, Sil, TICK_US};
use voyant::{SignalId, Value};

#[test]
fn tasks_advance() {
    const COUNTERS: [&str; 4] = ["task1msRuns", "task10msRuns", "taskUsbRuns", "telemRuns"];
    const N: u64 = 50;

    let mut world = Sil::new();

    // Fresh from reset: the firmware clock starts at 0. (The heartbeat counters are
    // near-0 too — boot leaves the USB task at 1 — so the window is measured as a
    // delta, not an absolute.)
    let before: Vec<u64> = COUNTERS
        .iter()
        .map(|c| world.fw().read_cvar(c).as_u64().unwrap_or(0))
        .collect();
    let timebase_before = world
        .fw()
        .read_cvar("lib_timer_data.currentTime_us")
        .as_u64()
        .unwrap_or(0);
    assert_eq!(timebase_before, 0, "firmware clock starts at 0 from reset");

    let ids: Vec<SignalId> = COUNTERS.iter().map(|c| cvar(c)).collect();
    // No per-signal declaration: the FirmwareMember auto-mirrors the whole cvar
    // namespace, so these counters are sampled into the historian each tick.
    let fwm = world.firmware_member();
    world.sim.add_member(fwm);
    for _ in 0..N {
        world.sim.step().expect("engine step");
    }
    // Post-window counter values come from the engine's own historian (auto-mirrored).
    let after: Vec<u64> = ids
        .iter()
        .map(|id| {
            world
                .sim
                .read(id.as_str())
                .ok()
                .flatten()
                .as_ref()
                .and_then(Value::as_u64)
                .unwrap_or(0)
        })
        .collect();
    let d: Vec<u64> = before.iter().zip(&after).map(|(b, a)| a.saturating_sub(*b)).collect();

    // 1 ms task fires once per tick; 10 ms every 10; telemetry every 2 ms; USB
    // delays 1 tick per iteration (so ~once per tick). Tolerance bands, not exact
    // equality (scheduling phase can shift a fire in or out of the window).
    assert!((45..=55).contains(&d[0]), "task1msRuns +{}", d[0]);
    assert!((3..=7).contains(&d[1]), "task10msRuns +{}", d[1]);
    assert!((40..=60).contains(&d[2]), "taskUsbRuns +{}", d[2]);
    assert!((20..=30).contains(&d[3]), "telemRuns +{}", d[3]);

    // The sim timebase flows: TIM2 advances with sim time, so lib_timer's
    // accumulated microseconds equal exactly one tick per step (the clock behind the
    // alignment dwell and the button tap/hold gestures).
    let timebase_after = world
        .sim
        .read(&cid("lib_timer_data.currentTime_us"))
        .ok()
        .flatten()
        .as_ref()
        .and_then(Value::as_u64)
        .unwrap_or(0);
    assert_eq!(
        timebase_after,
        N * TICK_US,
        "lib_timer is +1000 us/tick from reset (currentTime_us after {N} ticks)"
    );
}
