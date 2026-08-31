//! GPIO input sampling and EXTI edge dispatch, asserted white-box against the real
//! firmware: the sim driver's injected level array is the injection point (DWARF
//! write of `inputLevel`) and its cached snapshot / per-port edge counter are the
//! observation points, so the driver carries no test-only API.

use pcs_bldc_sil::{Sil, SOURCE};
use voyant::Value;

/// Port B's index in the `HW_GPIO_port_E`-shaped arrays.
const PORT_B: usize = 1;

/// PB10 — the mode button, a polled input in the board config.
const INPUT_BIT: usize = 10;

/// PB6 — an interrupt-mode pin in the board config (EXTI line 6).
const IRQ_BIT: usize = 6;

/// `HW_GPIO_level_E` ordinals — enumerator names do not resolve in this build
/// (see `docs/sil/backlog.md`), so levels are written as raw ordinals.
const LOW: u32 = 0;
const HIGH: u32 = 1;

/// A booted world with the firmware member added and one sampling pass behind it.
fn booted() -> Sil {
    let mut sim = Sil::new();
    let fwm = sim.load_firmware(SOURCE);
    sim.add_member(fwm);
    sim.run_for_ms(1);
    sim
}

/// Inject one pin's level into the sim driver.
fn set_level(sim: &Sil, port: usize, bit: usize, level: u32) {
    sim.fw().write_cvar(
        &format!("HW_GPIO_data.inputLevel[{port}][{bit}]"),
        &Value::U32(level),
    );
}

/// One bit of a port's polled-input cache — the state behind `HW_GPIO_readCached`.
fn cached_bit(sim: &Sil, port: usize, bit: usize) -> bool {
    let path = format!("HW_GPIO_data.cachedInput[{port}]");
    let cached = sim
        .fw()
        .read_cvar(&path)
        .as_u64()
        .unwrap_or_else(|| panic!("{path} reads as an unsigned mask"));
    (cached & (1 << bit)) != 0
}

/// A port's dispatched-edge count.
fn edge_count(sim: &Sil, port: usize) -> u64 {
    let path = format!("HW_GPIO_data.extiEdgeCount[{port}]");
    sim.fw()
        .read_cvar(&path)
        .as_u64()
        .unwrap_or_else(|| panic!("{path} reads as an unsigned count"))
}

// [test->fw~hal_gpio_004~1]
#[test]
fn a_cached_read_follows_the_injected_level_one_pass_later() {
    let mut sim = booted();
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
fn an_injected_level_change_dispatches_exactly_one_edge() {
    let mut sim = booted();
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

    // Falling change: one more.
    set_level(&sim, PORT_B, IRQ_BIT, LOW);
    sim.run_for_ms(3);
    assert_eq!(
        edge_count(&sim, PORT_B) - base,
        2,
        "the falling change dispatches one more edge"
    );

    // The interrupt pin never lands in the polled cache.
    assert!(
        !cached_bit(&sim, PORT_B, IRQ_BIT),
        "an interrupt pin is not a polled input"
    );
}
