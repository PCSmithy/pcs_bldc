//! Shared helpers for the `tests/*.rs` scenarios: world setup, the DWARF
//! read/write accessors every white-box suite needs, and the driver status
//! tables. Each integration test is its own crate, so this module is compiled
//! once per binary and most of it is unused in any single one.
#![allow(dead_code)]

use pcs_bldc_sil::board::{
    fault_latched, ALIGN_DWELL_MS, DIAL, GATE_BRINGUP_MS, GPIO_LEVEL_HIGH, GPIO_LEVEL_LOW,
    INPUT_LEVEL_PB10, MOTOR,
};
use pcs_bldc_sil::{cid, vid, Sil, SOURCE};
use voyant::Value;

/// The PWM period at 20 kHz — the control-rate grid a board world steps on.
pub const GRID_US: u64 = 50;

/// The kernel-tick handler the fiber port registers with the interrupt table at
/// scheduler start. Shared so the literal stays in sync across the suites.
pub const SYSTICK_ISR: &str = "vSilSysTickHandler";

/// A booted world: firmware loaded, added as a member, `ms` of run behind it.
pub fn booted(ms: u64) -> Sil {
    let mut sim = Sil::new();
    let fwm = sim.load_firmware(SOURCE);
    sim.add_member(fwm);
    sim.run_for_ms(ms);
    sim
}

/// An unsigned firmware field — a count, a register byte, a bit mask.
pub fn u64_at(sim: &Sil, path: &str) -> u64 {
    sim.fw()
        .read_cvar(path)
        .as_u64()
        .unwrap_or_else(|| panic!("{path} reads as an unsigned value"))
}

/// A floating-point firmware field — a decoded engineering value.
pub fn f64_at(sim: &Sil, path: &str) -> f64 {
    sim.fw()
        .read_cvar(path)
        .as_f64()
        .unwrap_or_else(|| panic!("{path} reads as a floating-point value"))
}

/// One of the plant's own signals, the physical truth a firmware reading is
/// measured against.
pub fn plant(sim: &Sil, local: &str) -> f64 {
    sim.read_f64(&vid(MOTOR, local))
}

/// Press and release the mode button, then wait out the double-tap window.
pub fn tap_button(sim: &mut Sil) {
    sim.write(&cid(INPUT_LEVEL_PB10), GPIO_LEVEL_LOW).expect("button press");
    sim.run_for_ms(30);
    sim.write(&cid(INPUT_LEVEL_PB10), GPIO_LEVEL_HIGH).expect("button release");
    sim.run_for_ms(30);
    sim.run_for_ms(320); // APP_USERCONTROLS_DOUBLE_TAP_WINDOW_MS plus slack
}

/// Bring the gate driver up, align, and dial the shaft to a steady spin — the
/// operating point the current-sense suites measure against.
pub fn spin_up(sim: &mut Sil) {
    sim.run_for_ms(GATE_BRINGUP_MS);
    tap_button(sim);
    sim.run_for_ms(ALIGN_DWELL_MS + 100);
    assert!(
        sim.read_bool(&cid("app_motorControl_data.channels[0].isAligned")),
        "alignment latched before the spin"
    );
    let mut deg = 0.0;
    while deg < 90.0 {
        deg += 10.0;
        sim.write(&vid(DIAL, "angle[deg]"), deg).expect("turn the dial");
        sim.run_for_ms(20);
    }
    sim.run_for_ms(300);
    let velocity = plant(sim, "velocity");
    assert!(velocity.abs() > 2.0, "the shaft is spinning, got {velocity:.2} rad/s");
    assert!(!fault_latched(sim), "no fault through the spin-up");
}

/// A `bool` firmware field.
pub fn bool_at(sim: &Sil, path: &str) -> bool {
    match sim.fw().read_cvar(path) {
        Value::Bool(b) => b,
        other => panic!("{path} reads as a bool, got {other:?}"),
    }
}

/// Write a `bool` firmware field — the drivers' fault knobs and world state.
pub fn set_bool(sim: &Sil, path: &str, value: bool) {
    sim.fw().write_cvar(path, &Value::Bool(value));
}

/// Write a `uint32_t` firmware field.
pub fn set_u32(sim: &Sil, path: &str, value: u32) {
    sim.fw().write_cvar(path, &Value::U32(value));
}

/// One value of a driver's status enum. Enumerator names do not resolve in this
/// build (`docs/sil/backlog.md`), so a status matches by name OR by the DWARF
/// reader's `<ordinal>` placeholder. The ordinals below mirror the C enums
/// positionally: an appended enumerator is safe, a reorder is silent — delete the
/// tables and the ordinal arm once the backend resolves enumerator names.
pub struct Status {
    pub name: &'static str,
    pub ordinal: i64,
}

/// `HW_ADC_conversionStatus_E` — a pass that stored every enabled input.
pub const ADC_OK: Status = Status {
    name: "HW_ADC_CONVERSION_STATUS_OK",
    ordinal: 2,
};
/// `HW_ADC_conversionStatus_E` — a stalled channel's pass, the sim stand-in for a
/// poll timeout.
pub const ADC_FAULT: Status = Status {
    name: "HW_ADC_CONVERSION_STATUS_FAULT",
    ordinal: 3,
};

/// `HW_SPI_status_E` — a successful transfer.
pub const SPI_COMPLETE: Status = Status {
    name: "HW_SPI_STATUS_COMPLETE",
    ordinal: 2,
};
/// `HW_SPI_status_E` — what a stalled or force-errored transfer leaves behind.
pub const SPI_ERROR: Status = Status {
    name: "HW_SPI_STATUS_ERROR",
    ordinal: 3,
};

/// `HW_DMA_status_E` — a channel with no transfer since init.
pub const DMA_IDLE: Status = Status {
    name: "HW_DMA_STATUS_IDLE",
    ordinal: 0,
};
/// `HW_DMA_status_E` — the status a successful completion lands.
pub const DMA_COMPLETE: Status = Status {
    name: "HW_DMA_STATUS_COMPLETE",
    ordinal: 2,
};
/// `HW_DMA_status_E` — the status a force-errored completion lands.
pub const DMA_ERROR: Status = Status {
    name: "HW_DMA_STATUS_ERROR",
    ordinal: 3,
};

/// Assert the status enum at `path`, read out of firmware memory past the historian.
pub fn assert_status(sim: &Sil, path: &str, want: &Status, why: &str) {
    let got = match sim.fw().read_cvar(path) {
        Value::Enum(name) => name,
        other => panic!("{path} reads as an enum, got {other:?}"),
    };
    assert!(
        (got == want.name) || (got == format!("<{}>", want.ordinal)),
        "{why}: {path} is {got}, expected {}",
        want.name
    );
}
