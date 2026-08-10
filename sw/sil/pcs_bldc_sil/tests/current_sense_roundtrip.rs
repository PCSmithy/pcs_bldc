//! The sense chain end to end through the firmware: injected plant currents reach the
//! ADC as pin volts and decode back to the amps injected, fault-level current decodes as
//! the rail, and a stuck sense line hides real current from the firmware.

use pcs_bldc_sil::board::{
    board, decoded_bus_a, decoded_phase_a, fault_latched, GATE_BRINGUP_MS, PHASE_TRIP_A, SENSE,
};
use pcs_bldc_sil::{vid, Board, Sil};
use voyant::SignalId;

/// The 1 ms task needs a few passes to resample after an injection.
const SETTLE_TICKS: u64 = 10;

/// Decode slack from 12-bit quantization over the 3.3 V rail (~0.8 mV/LSB → ~8 mA on a
/// 0.1 V/A phase shunt, ~1.4 mA on the 0.6 V/A bus shunt): a few LSB per channel.
const PHASE_TOL_A: f64 = 0.025;
const BUS_TOL_A: f64 = 0.010;

fn cvar_f64(sim: &Sil, path: &str) -> f64 {
    sim.fw()
        .read_cvar(path)
        .as_f64()
        .unwrap_or_else(|| panic!("{path} has no f64 value"))
}

/// A booted board world with the four motor → sense routes suspended, so the sense
/// inputs take direct writes while the four sense → ADC routes stay live.
fn world() -> (Sil, Vec<(SignalId, SignalId)>) {
    let Board { mut sim, sense, .. } = board(0.0);
    let plant: Vec<(SignalId, SignalId)> = sense.routes()[..4].to_vec();
    for (src, dst) in &plant {
        sim.suspend_route(src, dst)
            .unwrap_or_else(|e| panic!("suspend {src} -> {dst}: {e}"));
    }
    sim.run_for_ms(GATE_BRINGUP_MS);
    (sim, sense.routes().to_vec())
}

/// Command the four sense inputs (A) and let the firmware resample.
fn inject(sim: &mut Sil, phase_a: [f64; 3], bus_a: f64) {
    for (local, amps) in ["i_u", "i_v", "i_w"].iter().zip(phase_a) {
        sim.write(&vid(SENSE, local), amps)
            .unwrap_or_else(|e| panic!("write {local}: {e}"));
    }
    sim.write(&vid(SENSE, "i_bus"), bus_a).expect("write i_bus");
    sim.run_for_ms(SETTLE_TICKS);
}

// [test->fw~hal_adc_004~1]
// [test->fw~hal_adc_005~1]
// [test->fw~safety_001~1]
#[test]
fn firmware_decodes_injected_currents() {
    // The acceptance case: amps injected at the shunt come back out of the firmware's
    // decode as the same amps, locking the model's front-end parameters to its constants.
    const PHASE_A: [f64; 3] = [1.5, -1.2, 0.0];
    const BUS_A: f64 = 0.5;
    const TRIP_A: f64 = 2.5;

    let (mut sim, _routes) = world();
    assert!(!fault_latched(&sim), "the boot baseline is fault free");
    inject(&mut sim, PHASE_A, BUS_A);

    let decoded: Vec<f64> = (0..3).map(|k| decoded_phase_a(&sim, k)).collect();
    let decoded_bus = decoded_bus_a(&sim);
    println!(
        "firmware_decodes_injected_currents: injected {PHASE_A:?} A / {BUS_A} A bus -> \
         decoded {decoded:?} A / {decoded_bus:.6} A bus (phase band ±{PHASE_TOL_A} A, bus \
         band ±{BUS_TOL_A} A)"
    );

    for ((got, want), name) in decoded.iter().zip(PHASE_A).zip(["U", "V", "W"]) {
        assert!(
            (got - want).abs() < PHASE_TOL_A,
            "phase {name} decodes the {want} A injected within {PHASE_TOL_A} A: {got} A"
        );
    }
    assert!(
        (decoded_bus - BUS_A).abs() < BUS_TOL_A,
        "the bus decodes the {BUS_A} A injected within {BUS_TOL_A} A: {decoded_bus} A"
    );
    assert!(
        !fault_latched(&sim),
        "currents under the {PHASE_TRIP_A} A trip leave the latch clear"
    );

    // The firmware runs the same decode itself: pushing one phase over the trip latches
    // the overcurrent fault, so its constants match the ones read back above.
    inject(&mut sim, [TRIP_A, 0.0, 0.0], 0.0);
    assert!(
        fault_latched(&sim),
        "{TRIP_A} A injected on phase U trips the firmware's own {PHASE_TRIP_A} A decode \
         comparison"
    );
}

// [test->fw~hal_adc_005~1]
#[test]
fn firmware_sees_full_scale_on_saturation() {
    // Past full scale the amplifier rails, so the firmware reads the rail current rather
    // than the true one — it can tell the fault is large, never how large.
    const INJECTED_A: f64 = 30.0;

    let (mut sim, _routes) = world();
    inject(&mut sim, [INJECTED_A, 0.0, 0.0], 0.0);

    let vref = cvar_f64(&sim, "HW_ADC_channelConfig[0].vref");
    let bias_v = cvar_f64(
        &sim,
        "IO_bridge_channelConfig[0].phaseCurrent[0].zeroCurrentBias_V",
    );
    let volts_per_amp = cvar_f64(
        &sim,
        "IO_bridge_channelConfig[0].phaseCurrent[0].voltsPerAmp",
    );
    let rail_a = (vref - bias_v) / volts_per_amp;
    let decoded = decoded_phase_a(&sim, 0);
    println!(
        "firmware_sees_full_scale_on_saturation: injected {INJECTED_A} A -> decoded \
         {decoded:.6} A against a {rail_a:.6} A rail; overcurrent latched = {}",
        fault_latched(&sim)
    );

    assert!(
        (decoded - rail_a).abs() < PHASE_TOL_A,
        "phase U decodes the (vref - bias) / gain = {rail_a} A rail within {PHASE_TOL_A} A, \
         not the {INJECTED_A} A injected: {decoded} A"
    );
    assert!(
        fault_latched(&sim),
        "a railed phase is far over the {PHASE_TRIP_A} A trip and latches the fault"
    );
}

#[test]
fn stuck_sensor_hides_current() {
    // A sense line stuck at the zero-current midpoint is the dangerous fault: real winding
    // current keeps flowing while the firmware decodes zero, and the model shows the divergence.
    const INJECTED_A: f64 = 1.5;
    const U_ADC_PORT: &str = "vsig:pcs_bldc:ADC1_IN6";
    const U_VSENSE_PORT: &str = "vsig:current_sense:i_u_vsense";
    const STUCK_V: f64 = 1.65;

    let (mut sim, routes) = world();
    inject(&mut sim, [INJECTED_A, 0.0, 0.0], 0.0);
    let healthy = decoded_phase_a(&sim, 0);
    let healthy_v = sim.read_f64(U_VSENSE_PORT);
    assert!(
        (healthy - INJECTED_A).abs() < PHASE_TOL_A,
        "the healthy chain decodes the {INJECTED_A} A injected: {healthy} A"
    );

    // Cut the U sense line loose from the pin and hold the pin at the midpoint.
    let (src, dst) = &routes[4];
    sim.suspend_route(src, dst)
        .unwrap_or_else(|e| panic!("suspend {src} -> {dst}: {e}"));
    sim.write(U_ADC_PORT, STUCK_V)
        .expect("stick the U sense pin");
    sim.run_for_ms(SETTLE_TICKS);

    let hidden = decoded_phase_a(&sim, 0);
    let hidden_v = sim.read_f64(U_VSENSE_PORT);
    println!(
        "stuck_sensor_hides_current: healthy decode {healthy:.6} A at {healthy_v:.6} V -> \
         stuck decode {hidden:.6} A while the amplifier still puts out {hidden_v:.6} V"
    );
    assert!(
        hidden.abs() < PHASE_TOL_A,
        "the pin held at {STUCK_V} V decodes as no current: {hidden} A"
    );
    assert_eq!(
        hidden_v, healthy_v,
        "the amplifier output still carries the true current the firmware cannot see: \
         {hidden_v} V"
    );
    assert!(
        !fault_latched(&sim),
        "a hidden current raises no fault — nothing in the firmware sees it"
    );

    // Reconnecting the line hands the true current back to the decode.
    sim.resume_route(src, dst)
        .unwrap_or_else(|e| panic!("resume {src} -> {dst}: {e}"));
    sim.run_for_ms(SETTLE_TICKS);
    let restored = decoded_phase_a(&sim, 0);
    assert!(
        (restored - INJECTED_A).abs() < PHASE_TOL_A,
        "the reconnected line decodes the {INJECTED_A} A still flowing: {restored} A"
    );
}
