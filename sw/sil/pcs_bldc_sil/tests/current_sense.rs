//! The current-sense model's contract in a model-only world: port registration with
//! canonical units, the affine transfer (zero current included), and the amplifier rails
//! that saturate fault-level current and swallow regen on the bus.

use pcs_bldc_sil::board::SENSE;
use pcs_bldc_sil::{vid, CurrentSenseModel, CurrentSenseParams, Sil};
use voyant::vsig_id;

const PHASE_INPUTS: [&str; 3] = ["i_u", "i_v", "i_w"];
const PHASE_OUTPUTS: [&str; 3] = ["i_u_vsense", "i_v_vsense", "i_w_vsense"];

/// A model-only world holding the four input currents, stepped until the outputs carry
/// them. Returns the world and the parameters the model runs with.
fn driven(phase_a: [f64; 3], bus_a: f64) -> (Sil, CurrentSenseParams) {
    let params = CurrentSenseParams::default();
    let mut sim = Sil::new();
    sim.add_member(CurrentSenseModel::new(SENSE));
    for (local, amps) in PHASE_INPUTS.iter().zip(phase_a) {
        sim.write(&vid(SENSE, local), amps)
            .unwrap_or_else(|e| panic!("write {local}: {e}"));
    }
    sim.write(&vid(SENSE, "i_bus"), bus_a).expect("write i_bus");
    for _ in 0..3 {
        sim.step().expect("engine step");
    }
    (sim, params)
}

#[test]
fn ports_register_with_units() {
    // All eight ports exist after enable, carrying the canonical units of what they
    // stand for: currents in amps on the way in, pin voltages in volts on the way out.
    let mut sim = Sil::new();
    sim.add_member(CurrentSenseModel::new(SENSE));
    sim.step().expect("engine step");

    for (local, unit) in [
        ("i_u", "A"),
        ("i_v", "A"),
        ("i_w", "A"),
        ("i_bus", "A"),
        ("i_u_vsense", "V"),
        ("i_v_vsense", "V"),
        ("i_w_vsense", "V"),
        ("i_bus_vsense", "V"),
    ] {
        let id = vsig_id(SENSE, local).expect("valid vsig id");
        assert_eq!(
            sim.state().unit_of(&id),
            Some(unit),
            "{local} registered with unit {unit}"
        );
    }
}

#[test]
fn transfer_is_affine() {
    // Inside the rails every channel is exactly v = bias + gain · i, either sign of phase
    // current — the affine law the firmware's decode inverts.
    const PHASE_A: [f64; 3] = [3.0, -3.0, 0.5];
    const BUS_A: f64 = 2.0;

    let (sim, params) = driven(PHASE_A, BUS_A);
    for (local, amps) in PHASE_OUTPUTS.iter().zip(PHASE_A) {
        let v = sim.read_f64(&vid(SENSE, local));
        let expected = params.phase_bias_v + params.phase_gain_v_per_a * amps;
        assert_eq!(
            v, expected,
            "{local} carries bias + gain · {amps} A = {expected} V, got {v} V"
        );
    }
    let v_bus = sim.read_f64(&vid(SENSE, "i_bus_vsense"));
    let expected_bus = params.bus_bias_v + BUS_A * params.bus_gain_v_per_a;
    assert_eq!(
        v_bus, expected_bus,
        "the bus pin carries bias + gain · {BUS_A} A = {expected_bus} V, got {v_bus} V"
    );

    // The i = 0 case of the same law: each output sits exactly at its channel's bias —
    // the phase midpoint on the phase pins, ground on the (ground-referenced) bus pin.
    let (zero, params) = driven([0.0; 3], 0.0);
    for local in PHASE_OUTPUTS {
        let v = zero.read_f64(&vid(SENSE, local));
        assert_eq!(
            v, params.phase_bias_v,
            "zero current reads the phase midpoint on {local}: {v} V"
        );
    }
    let v_bus = zero.read_f64(&vid(SENSE, "i_bus_vsense"));
    assert_eq!(
        v_bus, params.bus_bias_v,
        "zero current reads ground on the bus pin: {v_bus} V"
    );
}

#[test]
fn saturation_rails() {
    // Fault-level current drives the amplifiers into their rails: the pin voltage pins at
    // vref or ground and carries no information about how far past full scale the current went.
    const PHASE_A: [f64; 3] = [20.0, -20.0, 0.0];
    const BUS_A: f64 = 20.0;

    let (sim, params) = driven(PHASE_A, BUS_A);
    let v_high = sim.read_f64(&vid(SENSE, "i_u_vsense"));
    let v_low = sim.read_f64(&vid(SENSE, "i_v_vsense"));
    let v_bus = sim.read_f64(&vid(SENSE, "i_bus_vsense"));
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

    let (sim, _params) = driven([0.0; 3], BUS_A);
    let v_bus = sim.read_f64(&vid(SENSE, "i_bus_vsense"));
    assert_eq!(
        v_bus, 0.0,
        "{BUS_A} A of regen reads 0 V on the bus pin, not a negative voltage: {v_bus} V"
    );
}
