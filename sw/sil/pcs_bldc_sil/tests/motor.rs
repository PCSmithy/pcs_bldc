//! The firmware world's motor loop: a zero-latency route carries the shaft angle into
//! the AS5048 model and the firmware, and [`wire_bridge`]'s delayed routes carry the
//! bridge command back into the plant. Covers unit registration, the angle hop, the SPI
//! poll, and bridge-route fault injection.
//!
//! [`wire_bridge`]: pcs_bldc_sil::wire_bridge

use pcs_bldc_sil::board::{board, COUNTS_PER_REV, MOTOR, VBUS_V};
use pcs_bldc_sil::{vid, Board, Sil, SOURCE};
use voyant::{vsig_id, SignalId};

#[test]
fn motor_scaffold_closes_the_loop() {
    const STUB_ANGLE_RAD: f64 = 0.75;
    let two_pi = 2.0 * std::f64::consts::PI;

    let Board { mut sim, .. } = board(STUB_ANGLE_RAD);

    // Motor outputs are registered with canonical units.
    let expected_units = [
        ("angle", "rad"),
        ("velocity", "rad/s"),
        ("phase_current_u", "A"),
        ("phase_current_v", "A"),
        ("phase_current_w", "A"),
        ("torque", "Nm"),
    ];
    for (local, unit) in expected_units {
        let id = vsig_id(MOTOR, local).expect("valid vsig id");
        assert_eq!(
            sim.state().unit_of(&id),
            Some(unit),
            "motor output {local} registered with unit {unit}"
        );
    }

    for _ in 0..3 {
        sim.step().expect("engine step");
    }

    // The shaft angle propagated motor → encoder: the encoder quantized it to the raw
    // count real hardware would report.
    let expected_raw = ((STUB_ANGLE_RAD * COUNTS_PER_REV) / two_pi).round() as u64;
    let raw = sim.read_u64("vsig:as5048_motor:raw_encoder_ticks");
    assert_eq!(
        raw, expected_raw,
        "angle route carries the shaft angle into the encoder"
    );

    // The firmware polled the encoder over the real SPI bus — the loop's last hop.
    let rx_id = SignalId::parse("spi:pcs_bldc:AS5048_1:rx").expect("valid spi id");
    assert!(
        sim.state()
            .changes(&rx_id)
            .map(|c| !c.is_empty())
            .unwrap_or(false),
        "firmware ran SPI transfers against the encoder model"
    );
}

#[test]
fn bridge_route_suspension_injects_faults() {
    // Suspend-and-write on the bridge routes: with them live the motor's inputs mirror
    // the firmware's dark bridge; suspended, a hand-written command drives the plant
    // without touching the firmware side; resumed, the firmware owns the inputs again.
    const BOOT_TICKS: usize = 300;
    const INJECT_TICKS: usize = 20;
    const RELEASE_TICKS: usize = 10;
    const RELEASE_BUDGET_TICKS: usize = 500;
    const INJECTED_DUTY: f64 = 0.6;
    const CURRENT_FLOOR_A: f64 = 0.05;

    let Board {
        mut sim, bridge, ..
    } = board(0.0);

    for _ in 0..BOOT_TICKS {
        sim.step().expect("engine step");
    }
    let booted_duty = sim.read_f64(&vid(MOTOR, "duty_u"));
    let booted_moe = sim.read_f64(&vid(MOTOR, "moe"));
    assert_eq!(
        (booted_duty, booted_moe),
        (0.0, 0.0),
        "the live routes mirror the firmware's dark bridge into the motor: duty_u = \
         {booted_duty}, moe = {booted_moe}"
    );

    // Suspend the routes and command the plant by hand.
    bridge
        .suspend_all(&mut sim)
        .expect("suspend the bridge routes");
    for (local, value) in [
        ("v_bus", VBUS_V),
        ("duty_u", INJECTED_DUTY),
        ("enable_u", 1.0),
        ("enable_v", 1.0),
        ("moe", 1.0),
    ] {
        sim.write(&vid(MOTOR, local), value)
            .unwrap_or_else(|e| panic!("write {local}: {e}"));
    }
    let mut peak_current = 0.0f64;
    for _ in 0..INJECT_TICKS {
        sim.step().expect("engine step");
        peak_current = peak_current.max(sim.read_f64(&vid(MOTOR, "phase_current_u")).abs());
    }
    let injected_duty = sim.read_f64(&vid(MOTOR, "duty_u"));
    let firmware_duty = sim.read_f64(&vid(SOURCE, "PWM_U_duty"));
    println!(
        "bridge_route_suspension_injects_faults: injected duty_u = {injected_duty}, peak \
         |i_u| = {peak_current:.6} A, firmware PWM_U_duty = {firmware_duty}"
    );
    assert!(
        peak_current > CURRENT_FLOOR_A,
        "the injected bridge command drives the plant: peak |i_u| = {peak_current:.6} A"
    );
    assert_eq!(
        firmware_duty, 0.0,
        "injection writes the motor's inputs only; the firmware's own port still reads \
         its dark-bridge duty: {firmware_duty}"
    );

    // Resume: the firmware's dark bridge takes the inputs back. The plant rings down
    // first (the injection swung the rotor), and this world's historian carries the
    // default 1e-3 deadband, so a decaying tail's last sub-epsilon sample lingers in
    // the table — assertions here cannot be tighter than that deadband.
    const PARKED_TOL: f64 = 2.0e-3;
    bridge
        .resume_all(&mut sim)
        .expect("resume the bridge routes");
    let currents = |sim: &Sil| {
        [
            sim.read_f64(&vid(MOTOR, "phase_current_u")),
            sim.read_f64(&vid(MOTOR, "phase_current_v")),
            sim.read_f64(&vid(MOTOR, "phase_current_w")),
        ]
    };
    let parked = |sim: &Sil| {
        (sim.read_f64(&vid(MOTOR, "velocity")).abs() < PARKED_TOL)
            && currents(sim).iter().all(|i| i.abs() < PARKED_TOL)
    };
    let mut parked_at = None;
    for k in 0..RELEASE_BUDGET_TICKS {
        sim.step().expect("engine step");
        if parked(&sim) {
            parked_at = Some(k);
            break;
        }
    }
    let released_duty = sim.read_f64(&vid(MOTOR, "duty_u"));
    let released_moe = sim.read_f64(&vid(MOTOR, "moe"));
    println!(
        "  after resume: duty_u = {released_duty}, moe = {released_moe}, parked at \
         {parked_at:?} ticks, currents = {:?}",
        currents(&sim)
    );
    assert_eq!(
        (released_duty, released_moe),
        (0.0, 0.0),
        "the resumed routes hand the inputs back to the firmware: duty_u = \
         {released_duty}, moe = {released_moe}"
    );
    assert!(
        parked_at.is_some(),
        "the plant parks (every current and the velocity within the historian \
         deadband) inside {RELEASE_BUDGET_TICKS} ticks: {:?}",
        currents(&sim)
    );
    for _ in 0..RELEASE_TICKS {
        sim.step().expect("engine step");
        assert!(
            parked(&sim),
            "the parked plant stays dark: {:?}",
            currents(&sim)
        );
    }
}
