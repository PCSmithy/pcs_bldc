//! GPIO input sampling and EXTI edge dispatch, asserted white-box against the real
//! firmware: the sim driver's injected level array is the injection point (DWARF
//! write of `inputLevel`) and its cached snapshot / per-port edge counter are the
//! observation points, so the driver carries no test-only API.

mod common;
use common::{booted, set_u32, u64_at};
use pcs_bldc_sil::Sil;

/// Port B's index in the `HW_GPIO_port_E`-shaped arrays.
const PORT_B: usize = 1;

/// PB10 — the mode button, a polled input in the board config.
const INPUT_BIT: usize = 10;

/// PB6 — a rising-edge interrupt pin in the board config (EXTI line 6). It and
/// PC13 are the board's only interrupt pins, and both are rising-only, so the
/// falling and both-edge trigger types are covered at the unit level instead
/// (test_HW_GPIO.c::test_exti_dispatch_honours_the_trigger_edge).
const IRQ_BIT: usize = 6;

/// `HW_GPIO_level_E` ordinals — enumerator names do not resolve in this build
/// (see `docs/sil/backlog.md`), so levels are written as raw ordinals.
const LOW: u32 = 0;
const HIGH: u32 = 1;

/// Inject one pin's level into the sim driver.
fn set_level(sim: &Sil, port: usize, bit: usize, level: u32) {
    set_u32(sim, &format!("HW_GPIO_data.inputLevel[{port}][{bit}]"), level);
}

/// One bit of a port's polled-input cache — the state behind `HW_GPIO_readCached`.
fn cached_bit(sim: &Sil, port: usize, bit: usize) -> bool {
    let cached = u64_at(sim, &format!("HW_GPIO_data.cachedInput[{port}]"));
    (cached & (1 << bit)) != 0
}

/// A port's dispatched-edge count.
fn edge_count(sim: &Sil, port: usize) -> u64 {
    u64_at(sim, &format!("HW_GPIO_data.extiEdgeCount[{port}]"))
}

// [test->fw~hal_gpio_004~1]
#[test]
fn a_cached_read_follows_the_injected_level_one_pass_later() {
    let mut sim = booted(1);
    assert!(
        !cached_bit(&sim, PORT_B, INPUT_BIT),
        "an uninjected input samples low"
    );

    // Injected high: the next sampling pass captures it.
    set_level(&sim, PORT_B, INPUT_BIT, HIGH);
    sim.run_for_ms(2);
    assert!(
        cached_bit(&sim, PORT_B, INPUT_BIT),
        "a sampling pass captures the injected high"
    );

    // Back low: the cache follows on the next pass.
    set_level(&sim, PORT_B, INPUT_BIT, LOW);
    sim.run_for_ms(2);
    assert!(
        !cached_bit(&sim, PORT_B, INPUT_BIT),
        "a sampling pass captures the injected low"
    );
}

// [test->fw~hal_gpio_005~1]
#[test]
fn an_injected_level_change_dispatches_only_the_configured_edge() {
    let mut sim = booted(1);
    let base = edge_count(&sim, PORT_B);

    // Rising change: one dispatch, and a held level dispatches nothing more.
    set_level(&sim, PORT_B, IRQ_BIT, HIGH);
    sim.run_for_ms(3);
    assert_eq!(
        edge_count(&sim, PORT_B) - base,
        1,
        "a rising level change dispatches exactly one edge"
    );
    sim.run_for_ms(3);
    assert_eq!(
        edge_count(&sim, PORT_B) - base,
        1,
        "a held level dispatches no further edges"
    );

    // Falling change: the pin is configured rising-only, so the down edge is
    // seen and rejected rather than dispatched.
    set_level(&sim, PORT_B, IRQ_BIT, LOW);
    sim.run_for_ms(3);
    assert_eq!(
        edge_count(&sim, PORT_B) - base,
        1,
        "a rising-only pin dispatches nothing on the falling change"
    );

    // Rejected, not lost: the detector tracked the level down, so the next
    // rising change dispatches again.
    set_level(&sim, PORT_B, IRQ_BIT, HIGH);
    sim.run_for_ms(3);
    assert_eq!(
        edge_count(&sim, PORT_B) - base,
        2,
        "the rejected falling change left the pin armed"
    );

    // The interrupt pin never lands in the polled cache.
    assert!(
        !cached_bit(&sim, PORT_B, IRQ_BIT),
        "an interrupt pin is not a polled input"
    );
}
