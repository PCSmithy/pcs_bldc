//! USB-PD sink status at the system level (sys~pd_001) on the board world. The
//! CYPD3177 is its HPI register file on the sim I2C bus, which this suite holds
//! as the device would: the PD contract in force, the Type-C attach state, and
//! the VBUS rail measured at the device's 100 mV resolution. The firmware's
//! exposed status (`dev_CYPD3177`) is read back after its 200 ms fetch.

mod common;

use common::{bool_at, set_bool, set_u32, u64_at};
use pcs_bldc_sil::board::{board, MOTOR, VBUS_V};
use pcs_bldc_sil::{vid, Sil};

/// The CYPD3177's register file — BUS_1 (index 0), sole device on the bus.
const PD_DEV: &str = "HW_I2C_data.buses[0].devices[0]";
/// The firmware's exposed PD status (channel 0).
const PD: &str = "dev_CYPD3177_data.channels[0]";

/// HPI register offsets (CYPD3177 datasheet).
const DEVICE_MODE: usize = 0x0000;
const PD_STATUS: usize = 0x1008;
const TYPE_C_STATUS: usize = 0x100C;
const BUS_VOLTAGE: usize = 0x100D;
const CURRENT_PDO: usize = 0x1010;
const CURRENT_RDO: usize = 0x1014;

/// One 200 ms fetch plus slack.
const FETCH_MS: u64 = 250;

/// The device's state as its registers report it.
struct Device {
    contract: Option<(u32, u32)>, // (voltage mV, current mA)
    attached: bool,
    rail_v: f64,
}

/// Write `bytes` little-endian into the device's register file at `reg`.
fn write_reg(sim: &Sil, reg: usize, value: u32, bytes: usize) {
    for i in 0..bytes {
        set_u32(sim, &format!("{PD_DEV}.regMem[{}]", reg + i), (value >> (8 * i)) & 0xFF);
    }
}

/// Hold the device's registers at `device`: a fixed-supply PDO and its RDO for
/// the contract (USB PD: 50 mV / 10 mA fields), PD_STATUS bit 10 for an explicit
/// contract, TYPE_C_STATUS bit 0 for an attached partner, and the rail rounded to
/// the BUS_VOLTAGE register's 100 mV.
fn hold(sim: &mut Sil, device: &Device) {
    sim.write(&vid(MOTOR, "v_bus"), device.rail_v).expect("the VBUS rail");
    write_reg(sim, DEVICE_MODE, 0x02, 1);
    let (mv, ma) = device.contract.unwrap_or((0, 0));
    write_reg(sim, CURRENT_PDO, ((mv / 50) << 10) | (ma / 10), 4);
    write_reg(sim, CURRENT_RDO, ((ma / 10) << 10) | (ma / 10), 4);
    write_reg(sim, PD_STATUS, u32::from(device.contract.is_some()) << 10, 4);
    write_reg(sim, TYPE_C_STATUS, u32::from(device.attached), 1);
    write_reg(sim, BUS_VOLTAGE, (device.rail_v * 10.0).round() as u32, 1);
}

/// The exposed status matches `device`.
fn assert_exposed(sim: &Sil, device: &Device) {
    let exposed_v = u64_at(sim, &format!("{PD}.busVoltage_mV")) as f64 / 1000.0;
    println!(
        "pd_status: contract {:?} attached {} rail {:.2} V -> exposed active {} {} mV {} mA, type-C {:#04x}, VBUS {exposed_v:.2} V",
        device.contract,
        device.attached,
        device.rail_v,
        bool_at(sim, &format!("{PD}.contractActive")),
        u64_at(sim, &format!("{PD}.negotiatedVoltage_mV")),
        u64_at(sim, &format!("{PD}.negotiatedCurrent_mA")),
        u64_at(sim, &format!("{PD}.typeCStatus"))
    );
    assert!(bool_at(sim, &format!("{PD}.present")), "the responding controller reads available");
    assert_eq!(bool_at(sim, &format!("{PD}.contractActive")), device.contract.is_some(), "contract active");
    if let Some((mv, ma)) = device.contract {
        assert_eq!(u64_at(sim, &format!("{PD}.negotiatedVoltage_mV")), u64::from(mv), "negotiated voltage");
        assert_eq!(u64_at(sim, &format!("{PD}.negotiatedCurrent_mA")), u64::from(ma), "negotiated current");
    }
    assert_eq!(u64_at(sim, &format!("{PD}.typeCStatus")) & 1 == 1, device.attached, "Type-C attach state");
    assert!((exposed_v - device.rail_v).abs() <= 0.1, "VBUS within 0.1 V of the {:.2} V rail: {exposed_v:.3} V", device.rail_v);
}

// [test->sys~pd_001~1]
#[test]
fn the_exposed_pd_status_follows_the_device() {
    let mut sim = board(0.0).sim;
    sim.run_for_ms(FETCH_MS);

    let states = [
        Device { contract: Some((20_000, 3_000)), attached: true, rail_v: VBUS_V },
        Device { contract: Some((15_000, 1_500)), attached: true, rail_v: 15.04 },
        Device { contract: None, attached: true, rail_v: 5.07 },
        Device { contract: None, attached: false, rail_v: 0.0 },
    ];
    for device in &states {
        hold(&mut sim, device);
        sim.run_for_ms(FETCH_MS);
        assert_exposed(&sim, device);
    }

    // The controller stops answering: unavailable; answering again: available.
    set_bool(&sim, "HW_I2C_data.buses[0].stall", true);
    sim.run_for_ms(FETCH_MS);
    assert!(!bool_at(&sim, &format!("{PD}.present")), "an unresponsive controller reads unavailable");
    set_bool(&sim, "HW_I2C_data.buses[0].stall", false);
    sim.run_for_ms(FETCH_MS);
    assert!(bool_at(&sim, &format!("{PD}.present")), "a responding controller reads available again");
}
