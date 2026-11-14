//! The amplifiers onto the internal ADC path (fw~hal_opamp_002): with each
//! amplifier's input-pin volts set (world state, DWARF), the ADC's regular
//! conversion of that amplifier's internal input reads the pin volts times the
//! channel's configured gain, to within half a count, and the internal input's
//! port publishes the volts converted.

use pcs_bldc_sil::{vid, Sil, SOURCE};
use voyant::Value;

/// Each amplifier with the ADC channel and internal input (IN#) it drives:
/// VOPAMP1 on ADC1 IN13, VOPAMP2 on ADC2 IN16, VOPAMP3 on ADC2 IN18.
const ROUTES: [(usize, usize, usize); 3] = [(0, 0, 13), (1, 1, 16), (2, 1, 18)];

/// Pin volts per step, distinct per amplifier so a routing mixup cannot cancel.
const PIN_V: [[f64; 3]; 3] = [[0.10, 0.35, 0.60], [0.25, 0.80, 1.20], [1.50, 0.05, 0.45]];

fn f64_at(sim: &Sil, path: &str) -> f64 {
    sim.fw().read_cvar(path).as_f64().unwrap_or_else(|| panic!("{path} reads as a number"))
}

fn u64_at(sim: &Sil, path: &str) -> u64 {
    sim.fw().read_cvar(path).as_u64().unwrap_or_else(|| panic!("{path} reads as a count"))
}

// [test->fw~hal_opamp_002~1]
#[test]
fn an_internal_input_conversion_reads_pin_volts_times_the_configured_gain() {
    let mut sim = Sil::new();
    let fwm = sim.load_firmware(SOURCE);
    sim.add_member(fwm);
    sim.run_for_ms(5);

    let mut worst_lsb = 0.0f64;
    for pins in PIN_V {
        for (opamp, v) in pins.iter().enumerate() {
            sim.fw().write_cvar(&format!("HW_OPAMP_data.inputVolts[{opamp}]"), &Value::F32(*v as f32));
        }
        // A few regular passes (1 ms cadence, DMA completion) convert the new inputs.
        sim.run_for_ms(5);

        for (opamp, adc, input) in ROUTES {
            let gain = f64_at(&sim, &format!("HW_OPAMP_channelConfig[{opamp}].gain"));
            let vref = f64_at(&sim, &format!("HW_ADC_channelConfig[{adc}].vref"));
            let full_scale = ((1u64 << u64_at(&sim, &format!("HW_ADC_channelConfig[{adc}].numBits"))) - 1) as f64;
            let counts = u64_at(&sim, &format!("HW_ADC_data.channelData[{adc}].counts[{input}]")) as f64;
            let lsb = vref / full_scale;
            let read_v = counts * lsb;
            let want_v = pins[opamp] * gain;
            let err_lsb = (read_v - want_v).abs() / lsb;
            println!(
                "opamp: OPAMP{} {:.3} V x {gain} -> ADC{} IN{input} {counts} counts = {read_v:.4} V (want {want_v:.4} V, {err_lsb:.2} LSB)",
                opamp + 1,
                pins[opamp],
                adc + 1
            );
            assert!(err_lsb <= 0.5 + 1e-3, "OPAMP{}: {read_v} V read, {want_v} V wanted", opamp + 1);
            let published = sim.read_f64(&vid(SOURCE, &format!("ADC{}_VOPAMP{}", adc + 1, opamp + 1)));
            assert!((published - want_v).abs() < 1e-5, "OPAMP{}: the internal input publishes {published} V", opamp + 1);
            worst_lsb = worst_lsb.max(err_lsb);
        }
    }
    println!("opamp: worst conversion error {worst_lsb:.2} LSB");
}
