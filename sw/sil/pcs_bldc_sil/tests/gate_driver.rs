//! The STSPIN32G4 gate driver at the system level on the board world: the
//! configure pass leaves the device's registers at the configured values, each
//! STATUS fault condition reaches the firmware's exposed state, and a bridge
//! enable request while the driver does not report operational leaves the bridge
//! dark. The device is the sim I2C register file on its bus; every configured
//! value is the firmware's, read out of the image.

mod common;

use common::{accepted, bool_at, exchange, select_method, tap_button, u64_at, MC, MODE_FOC_VELOCITY, MODE_OFF};
use common::proto::{command, speed_setpoint};
use pcs_bldc_sil::board::{
    align_dwell_ms, board, fault_latched, gate_operational, port, GATEDRIVER_STATUS_LOCKED, GATE_BRINGUP_MS,
    I2C_STATUS_REG,
};
use pcs_bldc_sil::{cid, Sil};

/// The gate driver's register file — BUS_2 (index 1), sole device on the bus.
const GATE_DEV: &str = "HW_I2C_data.buses[1].devices[0]";
/// The firmware's exposed state for the gate driver (channel 0).
const GATE: &str = "dev_gateDriver_data.channels[0]";
/// The configured register values (`dev_gateDriver_channelConfig_S`).
const GATE_CONFIG: &str = "dev_gateDriver_channelConfig[0]";

/// STSPIN32G4 configuration registers (datasheet offsets) and the config field
/// that defines each.
const CONFIG_REGS: [(usize, &str); 4] = [(0x01, "powmng"), (0x02, "logic"), (0x07, "ready"), (0x08, "nfault")];

/// STATUS bits (datasheet positions) and the exposed flag each drives.
const STATUS_BITS: [(u32, &str); 5] = [
    (0, "vccUndervoltage"),
    (1, "thermalShutdown"),
    (2, "vdsProtection"),
    (3, "resetLatched"),
    (7, "locked"),
];

/// One 200 ms status pass plus slack.
const STATUS_PASS_MS: u64 = 250;

fn bridge_enabled(sim: &Sil) -> bool {
    bool_at(sim, &format!("{MC}.bridgeEnabled"))
}

// [test->sys~mc_002~1]
#[test]
fn the_configure_pass_leaves_the_defined_values_in_the_device() {
    let mut sim = board(0.0).sim;
    sim.run_for_ms(GATE_BRINGUP_MS);
    assert!(bool_at(&sim, &format!("{GATE}.configured")), "the firmware's readback over I2C verified every write");
    for (reg, field) in CONFIG_REGS {
        let defined = u64_at(&sim, &format!("{GATE_CONFIG}.{field}"));
        let held = u64_at(&sim, &format!("{GATE_DEV}.regMem[{reg}]"));
        println!("gate_driver: reg {reg:#04x} ({field}) holds {held:#04x}, defined {defined:#04x}");
        assert_eq!(held, defined, "register {reg:#04x} holds the defined {field}");
    }
}

// [test->sys~mc_003~1]
#[test]
fn each_status_condition_reaches_the_exposed_state() {
    // Each condition alone on a locked device, then the lock alone withdrawn: every
    // exposed flag follows its STATUS bit, and operational only on the clean lock.
    let mut sim = board(0.0).sim;
    sim.run_for_ms(GATE_BRINGUP_MS);
    assert!(gate_operational(&sim), "a clean locked device reads operational");
    let lock = GATEDRIVER_STATUS_LOCKED as u32;
    for (bit, field) in STATUS_BITS {
        let status = lock ^ (1u32 << bit);
        sim.write(&cid(I2C_STATUS_REG), status).expect("set the device STATUS");
        sim.run_for_ms(STATUS_PASS_MS);
        for (b, f) in STATUS_BITS {
            let want = (status >> b) & 1 == 1;
            assert_eq!(bool_at(&sim, &format!("{GATE}.{f}")), want, "STATUS {status:#04x}: {f} follows bit {b}");
        }
        assert!(!gate_operational(&sim), "STATUS {status:#04x} ({field}) is not operational");
    }
    sim.write(&cid(I2C_STATUS_REG), lock).expect("a clean locked device again");
    sim.run_for_ms(STATUS_PASS_MS);
    assert!(gate_operational(&sim), "the clean lock reads operational again");
}

// [test->sys~mc_005~1]
#[test]
fn an_enable_request_while_the_driver_is_not_operational_leaves_the_bridge_dark() {
    // VDS protection on the device: neither a button tap nor a host Command enables
    // the bridge. With the device clean again the standing request drives it.
    let mut sim = board(0.0).sim;
    sim.run_for_ms(GATE_BRINGUP_MS);
    select_method(&mut sim, MODE_FOC_VELOCITY);
    let vds = (GATEDRIVER_STATUS_LOCKED as u32) | (1u32 << 2);
    sim.write(&cid(I2C_STATUS_REG), vds).expect("VDS protection on the device");
    sim.run_for_ms(STATUS_PASS_MS);
    assert!(!gate_operational(&sim), "the driver reports not operational");

    let hold_dark = |sim: &mut Sil, what: &str| {
        for _ in 0..(align_dwell_ms(sim) + 100) {
            sim.run_for_ms(1);
            assert!(!bridge_enabled(sim), "{what} leaves the bridge disabled");
            assert_eq!(port(sim, "TIM1_MOE"), 0.0, "{what}: the master output enable stays off");
        }
    };
    tap_button(&mut sim);
    hold_dark(&mut sim, "the button enable request");
    let (reply, _) = exchange(&mut sim, 10, command(MODE_OFF, vec![]));
    assert!(accepted(&reply), "OFF withdraws the button's request");
    let (reply, _) = exchange(&mut sim, 11, command(MODE_FOC_VELOCITY, vec![speed_setpoint(0, 2.0, 0.0)]));
    assert!(accepted(&reply), "the host enable request is taken");
    hold_dark(&mut sim, "the host enable request");

    sim.write(&cid(I2C_STATUS_REG), GATEDRIVER_STATUS_LOCKED).expect("a clean locked device");
    sim.run_for_ms(STATUS_PASS_MS);
    assert!(gate_operational(&sim), "the driver reports operational again");
    assert!(bridge_enabled(&sim), "with the driver operational the standing request drives the bridge");
    assert!(!fault_latched(&sim), "no fault through the run");
}
