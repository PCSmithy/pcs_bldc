//! Sim OPAMP behavior through the white-box seam: the input-pin volts are poked over
//! DWARF before boot (they are world state, sampled at init — a started amplifier is
//! set-and-forget analog), and init computes each amplifier's output — input times the
//! board-configured gain — into the DWARF-readable output statics.

use pcs_bldc_sil::{dll_path, lock_world};
use voyant::{Firmware, Value};

/// Distinct per-channel pin volts, so a gain/channel mixup cannot cancel out.
const INPUT_V: [f64; 3] = [0.5, 0.25, 1.2];

// [test->fw~hal_opamp_001~1]
// [test->fw~hal_opamp_002~1]
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
        let gain = fw
            .read_cvar(&format!("HW_OPAMP_channelConfig[{k}].gain"))
            .as_f64()
            .expect("gain reads as a float");
        let out = fw
            .read_cvar(&format!("HW_OPAMP_data.outputVolts[{k}]"))
            .as_f64()
            .expect("output reads as a float");
        assert!(gain > 0.0, "channel {k} has a configured gain");
        assert!(
            (out - (v * gain)).abs() < 1e-4,
            "channel {k}: {v} V in x {gain} -> {out} V out"
        );
    }

    fw.shutdown();
}
