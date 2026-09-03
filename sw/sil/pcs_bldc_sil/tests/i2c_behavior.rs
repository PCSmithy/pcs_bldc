//! I2C register transfers asserted white-box against the real firmware's own bus
//! traffic: the sim driver's statics are the injection point (DWARF writes of the
//! per-bus fault knobs and the register file) and the observation point (DWARF reads
//! of the device slots), so the driver carries no test-only API. The gate driver's
//! 200 ms configure + status pass is the transfer engine under test.

mod common;
use common::{bool_at, set_bool, u64_at};
use pcs_bldc_sil::board::{board, gate_operational, GATE_BRINGUP_MS};
use pcs_bldc_sil::{cid, Sil};

/// The gate driver's device slot on the sim I2C register file — BUS_2 (index 1),
/// sole device on the bus — and the CYPD3177 HPI slot on BUS_1 (index 0).
const GATE_DEV: &str = "HW_I2C_data.buses[1].devices[0]";
const PD_DEV: &str = "HW_I2C_data.buses[0].devices[0]";

/// The 7-bit device addresses `IO_i2c_channels.c` wires.
const GATE_ADDR7: u64 = 0x47;
const PD_ADDR7: u64 = 0x08;

/// STSPIN32G4 config registers and the bytes `dev_gateDriver_config` programs into
/// them (each write-verified by readback during bringup).
const GATE_CONFIG_REGS: [(usize, u64); 4] = [
    (0x01, 0x01), // POWMNG
    (0x02, 0x73), // LOGIC
    (0x07, 0x09), // READY
    (0x08, 0x7F), // NFAULT
];

/// Set or clear one per-bus fault knob (`stall` / `forceError`) on the gate bus.
fn set_bus_fault(sim: &Sil, knob: &str, value: bool) {
    set_bool(sim, &format!("HW_I2C_data.buses[1].{knob}"), value);
}

/// The gate driver's own record of its last STATUS read attempt.
fn gate_status_ok(sim: &Sil) -> bool {
    sim.read_bool(&cid("dev_gateDriver_data.channels[0].statusOk"))
}

// [test->fw~hal_i2c_004~1]
#[test]
fn register_writes_land_and_reads_return_the_register_file() {
    let mut b = board(0.0);
    b.sim.run_for_ms(GATE_BRINGUP_MS);

    // The read side: the firmware decoded the seeded STATUS byte (reg 0x80,
    // 16-bit offset encoding) out of the register file and came up operational.
    assert!(
        gate_operational(&b.sim),
        "bringup decodes the seeded STATUS register over memRead"
    );

    // The write side: every configure-pass register write landed byte-exact in
    // the addressed device's register memory (and survived its readback verify).
    for (reg, want) in GATE_CONFIG_REGS {
        assert_eq!(
            u64_at(&b.sim, &format!("{GATE_DEV}.regMem[{reg}]")),
            want,
            "the configure pass stores reg {reg:#04x} via memWrite"
        );
    }
}

// [test->fw~hal_i2c_002~1]
#[test]
fn devices_are_addressed_independently_per_transfer() {
    let mut b = board(0.0);
    b.sim.run_for_ms(GATE_BRINGUP_MS);

    // Both polled devices allocated their own slot, keyed by the per-transfer
    // bus + 7-bit address — no registration, first contact claims the slot.
    assert!(bool_at(&b.sim, &format!("{GATE_DEV}.used")), "gate slot in use");
    assert!(bool_at(&b.sim, &format!("{PD_DEV}.used")), "PD slot in use");
    assert_eq!(u64_at(&b.sim, &format!("{GATE_DEV}.addr7")), GATE_ADDR7);
    assert_eq!(u64_at(&b.sim, &format!("{PD_DEV}.addr7")), PD_ADDR7);

    // The gate driver's POWMNG write reached only the device it addressed: the
    // CYPD3177's register file holds no byte at that offset.
    let (powmng_reg, powmng_val) = GATE_CONFIG_REGS[0];
    assert_eq!(
        u64_at(&b.sim, &format!("{GATE_DEV}.regMem[{powmng_reg}]")),
        powmng_val
    );
    assert_eq!(
        u64_at(&b.sim, &format!("{PD_DEV}.regMem[{powmng_reg}]")),
        0,
        "a write addressed to the gate driver lands nowhere else"
    );
}

// [test->fw~hal_i2c_003~1]
#[test]
fn a_stalled_bus_fails_transfers_until_released() {
    let mut b = board(0.0);
    b.sim.run_for_ms(GATE_BRINGUP_MS);
    assert!(gate_operational(&b.sim), "healthy bus brings the gate up");

    // Stalled: every transfer on the bus times out, so the next STATUS pass
    // fails and the gate driver records the failed read.
    set_bus_fault(&b.sim, "stall", true);
    b.sim.run_for_ms(GATE_BRINGUP_MS);
    assert!(!gate_status_ok(&b.sim), "a stalled bus fails the STATUS read");
    assert!(!gate_operational(&b.sim), "a failed read drops operational");

    // Released: the next pass reads STATUS again and the gate recovers.
    set_bus_fault(&b.sim, "stall", false);
    b.sim.run_for_ms(GATE_BRINGUP_MS);
    assert!(gate_status_ok(&b.sim), "releasing the stall restores transfers");
    assert!(gate_operational(&b.sim), "the gate is operational again");
}

// [test->fw~hal_i2c_003~1]
#[test]
fn a_bus_error_fails_transfers_until_cleared() {
    let mut b = board(0.0);
    b.sim.run_for_ms(GATE_BRINGUP_MS);
    assert!(gate_operational(&b.sim), "healthy bus brings the gate up");

    set_bus_fault(&b.sim, "forceError", true);
    b.sim.run_for_ms(GATE_BRINGUP_MS);
    assert!(!gate_status_ok(&b.sim), "a bus error fails the STATUS read");

    set_bus_fault(&b.sim, "forceError", false);
    b.sim.run_for_ms(GATE_BRINGUP_MS);
    assert!(gate_status_ok(&b.sim), "clearing the error restores transfers");
}
