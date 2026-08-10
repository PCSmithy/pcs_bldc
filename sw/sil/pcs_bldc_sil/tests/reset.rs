//! Firmware member reset lifecycle: disabling a firmware member holds it in reset
//! (memory frozen, sim time flows); re-enabling reboots a fresh image from the same
//! path — statics from reset, firmware clock back to 0 — on one continuous sim
//! timeline. The historian preserves the clock signal's history across the reload, so
//! its change-log is a sawtooth: 100 ms up, 100 ms dark, 100 ms up.

use pcs_bldc_sil::{cid, cvar, Sil, SOURCE, TICK_US};

/// The mirrored firmware clock, a `uint64_t` static swept into the State Table each
/// tick. Read out of the table (not DWARF), so it FREEZES while the member is disabled.
const CLOCK: &str = "lib_timer_data.currentTime_us";

/// Read the mirrored clock's current table value.
fn table_clock(sim: &Sil) -> u64 {
    sim.read(&cid(CLOCK))
        .expect("clock signal registered")
        .and_then(|v| v.as_u64())
        .expect("clock has a mirrored value")
}

#[test]
fn firmware_reset_lifecycle() {
    let mut sim = Sil::new();
    let fwm = sim.load_firmware(SOURCE);
    sim.add_member(fwm);

    // Sim advances one firmware tick (TICK_US) per ms of the 1 kHz cadence, so 100 ms
    // of life climbs the clock to 100 * TICK_US.
    let alive_us = 100 * TICK_US;
    let dark_ms = 100 * 1_000;

    // (a) 100 ms alive: the firmware clock climbs to 100 * TICK_US.
    sim.run_for_ms(100);
    assert_eq!(table_clock(&sim), alive_us, "clock after 100 ms alive");
    let now_life1_end = sim.now_us();

    // (b) Disabled 100 ms: held in reset — the mirrored clock is FROZEN (no sweep runs)
    // while sim time keeps flowing.
    assert!(
        sim.set_member_enabled(SOURCE, false),
        "member found to disable"
    );
    sim.run_for_ms(100);
    assert_eq!(table_clock(&sim), alive_us, "clock frozen while disabled");
    assert_eq!(
        sim.now_us(),
        now_life1_end + dark_ms,
        "sim time still advanced 100 ms while disabled"
    );

    // (c) Re-enable 100 ms: a fresh image boots from reset — the clock climbs from 0 to
    // 100 * TICK_US again, NOT cumulatively to 300 * TICK_US.
    assert!(
        sim.set_member_enabled(SOURCE, true),
        "member found to re-enable"
    );
    sim.run_for_ms(100);
    assert_eq!(
        table_clock(&sim),
        alive_us,
        "clock climbs from reset, not cumulative"
    );

    // (d) The historian for the clock spans BOTH lives — history preserved across the
    // reload's re-registration. Its change-log is the sawtooth.
    let stamped: Vec<(u64, u64)> = sim
        .state()
        .changes(&cvar(CLOCK))
        .expect("clock has recorded history")
        .iter()
        .map(|(t, v)| (*t, v.as_u64().expect("u64 clock sample")))
        .collect();

    // The frozen gap: no records land while the member is disabled.
    assert!(
        !stamped
            .iter()
            .any(|(t, _)| (*t > now_life1_end) && (*t <= now_life1_end + dark_ms)),
        "no clock records while disabled: {stamped:?}"
    );
    // Life 1 climbs to the peak before the gap.
    assert!(
        stamped
            .iter()
            .any(|(t, v)| (*t <= now_life1_end) && (*v == alive_us)),
        "life-1 climb reaches the peak: {stamped:?}"
    );
    // The reset edge: the first post-re-enable sample drops back to the first tick.
    let first_post = stamped
        .iter()
        .find(|(t, _)| *t > now_life1_end + dark_ms)
        .expect("a post-reset clock sample");
    assert!(
        first_post.1 <= TICK_US,
        "post-reset clock restarts low, got {}",
        first_post.1
    );
    // Life 2 climbs back to the peak.
    assert_eq!(
        stamped.last().expect("at least one sample").1,
        alive_us,
        "life-2 climb reaches the peak again"
    );

    // (e) On drop, Sil dumps `firmware_reset_lifecycle.mf4` when PCS_SIL_TRACE_DIR is
    // set (named after this test's thread) — the sawtooth as the morning artifact.
}
