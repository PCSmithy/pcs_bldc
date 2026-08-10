//! The current-sense model's contract in a model-only world: port registration with
//! canonical units, the zero-current baseline, the affine transfer, and the amplifier
//! rails that saturate fault-level current and swallow regen on the bus.

use pcs_bldc_sil::{CurrentSenseModel, CurrentSenseParams, TICK_US};
use voyant::Engine;

const SENSE: &str = "current_sense";
const PHASE_INPUTS: [&str; 3] = ["i_u", "i_v", "i_w"];
const PHASE_OUTPUTS: [&str; 3] = ["i_u_vsense", "i_v_vsense", "i_w_vsense"];

fn read(eng: &Engine, id: &str) -> f64 {
    eng.read(id)
        .unwrap_or_else(|e| panic!("read {id}: {e}"))
        .and_then(|v| v.as_f64())
        .unwrap_or_else(|| panic!("{id} has no value"))
}

/// A model-only world holding the four input currents, stepped until the outputs carry
/// them. Returns the engine and the parameters the model runs with.
fn driven(phase_a: [f64; 3], bus_a: f64) -> (Engine, CurrentSenseParams) {
    let params = CurrentSenseParams::default();
    let mut eng = Engine::new(TICK_US);
    eng.add_member(CurrentSenseModel::new(SENSE));
    for (local, amps) in PHASE_INPUTS.iter().zip(phase_a) {
        eng.write(&format!("vsig:{SENSE}:{local}"), amps)
            .unwrap_or_else(|e| panic!("write {local}: {e}"));
    }
    eng.write(&format!("vsig:{SENSE}:i_bus"), bus_a)
        .expect("write i_bus");
    for _ in 0..3 {
        eng.step().expect("engine step");
    }
    (eng, params)
}

#[test]
fn zero_current_reads_bias() {
    // Undriven inputs are zero current, so each output sits exactly at its channel's
    // bias: the phase midpoint on the phase pins, ground on the bus pin. This holds
    // for the scaffold and for the implemented transfer alike (i = 0 ⇒ v = bias).
    let params = CurrentSenseParams::default();
    let mut eng = Engine::new(TICK_US);
    eng.add_member(CurrentSenseModel::new(SENSE));
    for _ in 0..5 {
        eng.step().expect("engine step");
    }

    for c in ["i_u_vsense", "i_v_vsense", "i_w_vsense"] {
        let v = read(&eng, &format!("vsig:{SENSE}:{c}"));
        assert_eq!(
            v, params.phase_bias_v,
            "zero current reads the phase midpoint on {c}: {v} V"
        );
    }
    let v_bus = read(&eng, &format!("vsig:{SENSE}:i_bus_vsense"));
    assert_eq!(
        v_bus, params.bus_bias_v,
        "zero current reads ground on the bus pin: {v_bus} V"
    );
}

#[test]
fn ports_register_with_units() {
    // All eight ports exist after enable: current inputs writable in amps, voltage
    // outputs readable in volts.
    let mut eng = Engine::new(TICK_US);
    eng.add_member(CurrentSenseModel::new(SENSE));
    eng.step().expect("engine step");

    for local in ["i_u", "i_v", "i_w", "i_bus"] {
        eng.write(&format!("vsig:{SENSE}:{local}"), 1.0)
            .unwrap_or_else(|e| panic!("write {local}: {e}"));
    }
    for local in ["i_u_vsense", "i_v_vsense", "i_w_vsense", "i_bus_vsense"] {
        let v = read(&eng, &format!("vsig:{SENSE}:{local}"));
        assert!(v.is_finite(), "{local} reads a finite voltage: {v}");
    }
}

#[test]
fn transfer_is_affine() {
    // Inside the rails every channel is exactly v = bias + gain · i, either sign of phase
    // current — the affine law the firmware's decode inverts.
    const PHASE_A: [f64; 3] = [3.0, -3.0, 0.5];
    const BUS_A: f64 = 2.0;

    let (eng, params) = driven(PHASE_A, BUS_A);
    for (local, amps) in PHASE_OUTPUTS.iter().zip(PHASE_A) {
        let v = read(&eng, &format!("vsig:{SENSE}:{local}"));
        let expected = params.phase_bias_v + params.phase_gain_v_per_a * amps;
        assert_eq!(
            v, expected,
            "{local} carries bias + gain · {amps} A = {expected} V, got {v} V"
        );
    }
    let v_bus = read(&eng, &format!("vsig:{SENSE}:i_bus_vsense"));
    let expected_bus = params.bus_bias_v + BUS_A * params.bus_gain_v_per_a;
    assert_eq!(
        v_bus, expected_bus,
        "the bus pin carries bias + gain · {BUS_A} A = {expected_bus} V, got {v_bus} V"
    );
}

#[test]
fn saturation_rails() {
    // Fault-level current drives the amplifiers into their rails: the pin voltage pins at
    // vref or ground and carries no information about how far past full scale the current went.
    const PHASE_A: [f64; 3] = [20.0, -20.0, 0.0];
    const BUS_A: f64 = 20.0;

    let (eng, params) = driven(PHASE_A, BUS_A);
    let v_high = read(&eng, &format!("vsig:{SENSE}:i_u_vsense"));
    let v_low = read(&eng, &format!("vsig:{SENSE}:i_v_vsense"));
    let v_bus = read(&eng, &format!("vsig:{SENSE}:i_bus_vsense"));
    assert_eq!(
        (v_high, v_low),
        (params.vref_v, 0.0),
        "±20 A rails the phase pins at vref and ground: {v_high} V, {v_low} V"
    );
    assert_eq!(
        v_bus, params.vref_v,
        "{BUS_A} A rails the bus pin at vref: {v_bus} V"
    );
}

#[test]
fn regen_reads_zero() {
    // The ground-referenced bus amplifier has no negative headroom, so current flowing
    // back into the supply reads as no current at all.
    const BUS_A: f64 = -2.0;

    let (eng, _params) = driven([0.0; 3], BUS_A);
    let v_bus = read(&eng, &format!("vsig:{SENSE}:i_bus_vsense"));
    assert_eq!(
        v_bus, 0.0,
        "{BUS_A} A of regen reads 0 V on the bus pin, not a negative voltage: {v_bus} V"
    );
}
