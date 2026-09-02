//! Sim OPAMP behavior through the white-box seam: the input-pin volts are poked over
//! DWARF before boot (they are world state, sampled at init — a started amplifier is
//! set-and-forget analog), and init computes each amplifier's output — input times the
//! board-configured gain — into the DWARF-readable output statics.
//!
//! This is NOT coverage of `fw~hal_opamp_002~1`, whose acceptance is a *conversion* of
//! the amplifier's internal ADC input: the sim ADC reads its pin voltage from a SIL
//! port and has no route from `HW_OPAMP_data.outputVolts`, so that acceptance is
//! unreachable until the sim models the internal OPAMP -> ADC path.

use pcs_bldc_sil::{dll_path, lock_world};
use voyant::{Firmware, Value};

/// Distinct per-channel pin volts, so a gain/channel mixup cannot cancel out.
const INPUT_V: [f64; 3] = [0.5, 0.25, 1.2];

/// The PGA gain the board wires on every amplifier (`OPAMP_PGA_GAIN_2_OR_MINUS_1`).
/// Stated here rather than read back from the config under test, so a wrong board
/// gain fails instead of cancelling out.
const BOARD_GAIN: f64 = 2.0;

// [test->fw~hal_opamp_001~1]
#[test]
fn opamp_output_is_input_times_gain() {
    // Raw load -> poke -> start cycle (the lifecycle-spike pattern): statics are
    // writable from load, and boot is where the sim amplifier samples its input.
    let _guard = lock_world();
    let fw = Firmware::load(&dll_path()).expect("load firmware");
    for (k, v) in INPUT_V.iter().enumerate() {
        fw.write_cvar(
            &format!("HW_OPAMP_data.inputVolts[{k}]"),
            &Value::F32(*v as f32),
        );
    }

    assert!(fw.start(), "sil_fw_start");
    assert!(
        matches!(fw.read_cvar("HW_OPAMP_data.initialized"), Value::Bool(true)),
        "HW_OPAMP_init accepted the board config"
    );

    for (k, v) in INPUT_V.iter().enumerate() {
        let out = fw
            .read_cvar(&format!("HW_OPAMP_data.outputVolts[{k}]"))
            .as_f64()
            .expect("output reads as a float");
        assert!(
            (out - (v * BOARD_GAIN)).abs() < 1e-4,
            "channel {k}: {v} V in x {BOARD_GAIN} -> {out} V out"
        );
    }

    fw.shutdown();
}
