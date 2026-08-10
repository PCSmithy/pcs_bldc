//! The sense chain end to end through the firmware: injected plant currents reach the
//! ADC as pin volts and decode back to the amps injected, fault-level current decodes as
//! the rail, and a stuck sense line hides real current from the firmware.

use pcs_bldc_sil::{
    cid, wire_bridge, wire_current_sense, As5048Model, CurrentSenseModel, MotorModel, Sil, SOURCE,
};
use voyant::{SignalId, Value};

const MOTOR: &str = "motor";
const SENSE: &str = "current_sense";

/// Boot the firmware past the gate-driver bring-up, then let the 1 ms task resample.
const BOOT_TICKS: u64 = 300;
const SETTLE_TICKS: u64 = 10;

/// Decode slack from 12-bit quantization over the 3.3 V rail (~0.8 mV/LSB → ~8 mA on a
/// 0.1 V/A phase shunt, ~1.4 mA on the 0.6 V/A bus shunt): a few LSB per channel.
const PHASE_TOL_A: f64 = 0.025;
const BUS_TOL_A: f64 = 0.010;

/// `OVERCURRENT_PHASE_TRIP_A` in `app_motorControl.c` — injections stay under it so the
/// latch does not confound a decode reading.
const PHASE_TRIP_A: f64 = 2.0;

fn cvar_f64(sim: &Sil, path: &str) -> f64 {
    sim.fw()
        .read_cvar(path)
        .as_f64()
        .unwrap_or_else(|| panic!("{path} has no f64 value"))
}

fn mirror_f64(sim: &Sil, path: &str) -> f64 {
    sim.read(&cid(path))
        .unwrap_or_else(|e| panic!("read {path}: {e}"))
        .as_ref()
        .and_then(Value::as_f64)
        .unwrap_or_else(|| panic!("{path} has no value"))
}

fn port(sim: &Sil, id: &str) -> f64 {
    sim.read(id)
        .unwrap_or_else(|e| panic!("read {id}: {e}"))
        .as_ref()
        .and_then(Value::as_f64)
        .unwrap_or_else(|| panic!("{id} has no value"))
}

/// One phase's current in amps as the firmware itself decoded it: the value
/// `app_motorControl` cached from `IO_bridge_getPhaseCurrent` on its last 1 ms pass —
/// the executed decode, not a re-computation.
fn decoded_phase_amps(sim: &Sil, phase: usize) -> f64 {
    mirror_f64(
        sim,
        &format!("app_motorControl_data.channels[0].phaseCurrent_a[{phase}]"),
    )
}

/// The bus current in amps as the firmware itself decoded it.
fn decoded_bus_amps(sim: &Sil) -> f64 {
    mirror_f64(sim, "app_motorControl_data.channels[0].busCurrent")
}

fn fault_latched(sim: &Sil) -> bool {
    matches!(
        sim.read(&cid("app_motorControl_data.channels[0].faultLatched"))
            .ok()
            .flatten(),
        Some(Value::Bool(true))
    )
}

/// A booted firmware world with the motor, its encoder, the sense chain, and both wiring
/// bundles. The four motor → sense routes come back suspended, so the sense inputs take
/// direct writes while the four sense → ADC routes stay live.
fn world() -> (Sil, Vec<(SignalId, SignalId)>) {
    let mut sim = Sil::new();
    // Producer → sensor → firmware, so the zero-latency sense routes land the same tick.
    sim.add_member(MotorModel::new(MOTOR, 0.0));
    // The commutation encoder answers the firmware's SPI polls; without it the encoder
    // fault latches and masks the overcurrent latch these tests read.
    let encoder = sim.add_member(As5048Model::new("as5048_motor", 0.0));
    sim.add_member(CurrentSenseModel::new(SENSE));
    let fwm = sim.load_firmware(SOURCE);
    sim.add_member(fwm);
    sim.link_duplex("spi:pcs_bldc:AS5048_1", encoder)
        .expect("link the encoder to the firmware SPI");
    wire_bridge(&mut sim, SOURCE, MOTOR).expect("wire the firmware bridge into the motor");
    let sense = wire_current_sense(&mut sim, MOTOR, SENSE, SOURCE).expect("wire the sense chain");

    let plant: Vec<(SignalId, SignalId)> = sense.routes()[..4].to_vec();
    for (src, dst) in &plant {
        sim.suspend_route(src, dst)
            .unwrap_or_else(|e| panic!("suspend {src} -> {dst}: {e}"));
    }
    sim.run_for_ms(BOOT_TICKS);
    (sim, sense.routes().to_vec())
}

/// Command the four sense inputs (A) and let the firmware resample.
fn inject(sim: &mut Sil, phase_a: [f64; 3], bus_a: f64) {
    for (local, amps) in ["i_u", "i_v", "i_w"].iter().zip(phase_a) {
        sim.write(&format!("vsig:{SENSE}:{local}"), amps)
            .unwrap_or_else(|e| panic!("write {local}: {e}"));
    }
    sim.write(&format!("vsig:{SENSE}:i_bus"), bus_a)
        .expect("write i_bus");
    sim.run_for_ms(SETTLE_TICKS);
}

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

    let decoded: Vec<f64> = (0..3).map(|k| decoded_phase_amps(&sim, k)).collect();
    let decoded_bus = decoded_bus_amps(&sim);
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
    let decoded = decoded_phase_amps(&sim, 0);
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
    let healthy = decoded_phase_amps(&sim, 0);
    let healthy_v = port(&sim, U_VSENSE_PORT);
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

    let hidden = decoded_phase_amps(&sim, 0);
    let hidden_v = port(&sim, U_VSENSE_PORT);
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
    let restored = decoded_phase_amps(&sim, 0);
    assert!(
        (restored - INJECTED_A).abs() < PHASE_TOL_A,
        "the reconnected line decodes the {INJECTED_A} A still flowing: {restored} A"
    );
}
