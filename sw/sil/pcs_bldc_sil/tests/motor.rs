//! The firmware world's motor loop: a zero-latency route carries the shaft angle into
//! the AS5048 model and the firmware, and [`wire_bridge`]'s delayed routes carry the
//! bridge command back into the plant. Covers unit registration, the dark boot state,
//! the angle hop, and bridge-route fault injection.

use pcs_bldc_sil::{wire_bridge, As5048Model, MotorModel, Sil, SOURCE};
use voyant::{vsig_id, SignalId, Value};

/// One `vsig` read as an `f64`, panicking with the id when the signal has no value.
fn read(sim: &Sil, id: &str) -> f64 {
    sim.read(id)
        .unwrap_or_else(|e| panic!("read {id}: {e}"))
        .as_ref()
        .and_then(Value::as_f64)
        .unwrap_or_else(|| panic!("{id} has no value"))
}

#[test]
fn motor_scaffold_closes_the_loop() {
    const STUB_ANGLE_RAD: f64 = 0.75;
    const COUNTS_PER_REV: f64 = 16384.0;
    let two_pi = 2.0 * std::f64::consts::PI;

    let mut sim = Sil::new();
    // Producer → consumer → firmware: motor before encoder before firmware, so the
    // zero-latency angle route lands in the encoder the same tick the motor records it.
    sim.add_member(MotorModel::new("motor", STUB_ANGLE_RAD));
    let encoder = sim.add_member(As5048Model::new("as5048_motor", 0.0));
    let fwm = sim.load_firmware(SOURCE);
    sim.add_member(fwm);

    // Close the loop: the motor's shaft angle drives the encoder model's input, and the
    // firmware's bridge command drives the motor's inputs one tick later.
    sim.add_route(
        vsig_id("motor", "angle").expect("valid vsig id"),
        vsig_id("as5048_motor", "angle").expect("valid vsig id"),
    )
    .expect("route motor angle into the encoder");
    sim.link_duplex("spi:pcs_bldc:AS5048_1", encoder)
        .expect("link the encoder to the firmware SPI");
    wire_bridge(&mut sim, SOURCE, "motor").expect("wire the firmware bridge into the motor");

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
        let id = vsig_id("motor", local).expect("valid vsig id");
        assert_eq!(
            sim.state().unit_of(&id),
            Some(unit),
            "motor output {local} registered with unit {unit}"
        );
    }

    for _ in 0..3 {
        sim.step().expect("engine step");
    }

    // The firmware's seven bridge observation ports read the dark-bridge boot state
    // (gate driver unseeded → drive blocked → all zeros), and the motor's `advance`
    // reads exactly these ids.
    let pwm_ports = [
        "PWM_U_duty",
        "PWM_V_duty",
        "PWM_W_duty",
        "PWM_U_enabled",
        "PWM_V_enabled",
        "PWM_W_enabled",
        "TIM1_MOE",
    ];
    let non_zero: Vec<String> = pwm_ports
        .iter()
        .filter_map(|p| {
            let id = format!("vsig:{SOURCE}:{p}");
            match sim
                .read(&id)
                .ok()
                .flatten()
                .as_ref()
                .and_then(Value::as_f64)
            {
                Some(0.0) => None,
                other => Some(format!("{p}={other:?}")),
            }
        })
        .collect();
    assert!(
        non_zero.is_empty(),
        "PWM observation ports read 0.0 (dark bridge); non-zero: {non_zero:?}"
    );

    // The stub angle propagated motor → encoder: the encoder quantized it to the raw
    // count real hardware would report (no physics, yet data flows end to end).
    let expected_raw = ((STUB_ANGLE_RAD * COUNTS_PER_REV) / two_pi).round() as u64;
    let raw = sim
        .read("vsig:as5048_motor:raw_encoder_ticks")
        .ok()
        .flatten()
        .as_ref()
        .and_then(Value::as_u64)
        .expect("encoder recorded a raw count");
    assert_eq!(
        raw, expected_raw,
        "angle route carries the stub angle into the encoder"
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
    const INJECTED_DUTY: f64 = 0.6;
    const INJECTED_VBUS_V: f64 = 24.0;
    const CURRENT_FLOOR_A: f64 = 0.05;

    let mut sim = Sil::new();
    // Plant before firmware, so the bridge routes back into it are the backward edges
    // the delayed latency covers.
    sim.add_member(MotorModel::new("motor", 0.0));
    let fwm = sim.load_firmware(SOURCE);
    sim.add_member(fwm);
    let bridge = wire_bridge(&mut sim, SOURCE, "motor").expect("wire the firmware bridge");

    for _ in 0..BOOT_TICKS {
        sim.step().expect("engine step");
    }
    let booted_duty = read(&sim, "vsig:motor:duty_u");
    let booted_moe = read(&sim, "vsig:motor:moe");
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
    for (id, value) in [
        ("vsig:motor:vbus", INJECTED_VBUS_V),
        ("vsig:motor:duty_u", INJECTED_DUTY),
        ("vsig:motor:enable_u", 1.0),
        ("vsig:motor:enable_v", 1.0),
        ("vsig:motor:moe", 1.0),
    ] {
        sim.write(id, value)
            .unwrap_or_else(|e| panic!("write {id}: {e}"));
    }
    let mut peak_current = 0.0f64;
    for _ in 0..INJECT_TICKS {
        sim.step().expect("engine step");
        peak_current = peak_current.max(read(&sim, "vsig:motor:phase_current_u").abs());
    }
    let injected_duty = read(&sim, "vsig:motor:duty_u");
    let firmware_duty = read(&sim, &format!("vsig:{SOURCE}:PWM_U_duty"));
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

    // Resume: the firmware's dark bridge takes the inputs back and the legs demagnetize.
    bridge
        .resume_all(&mut sim)
        .expect("resume the bridge routes");
    for _ in 0..RELEASE_TICKS {
        sim.step().expect("engine step");
    }
    let released_duty = read(&sim, "vsig:motor:duty_u");
    let released_moe = read(&sim, "vsig:motor:moe");
    let currents = [
        read(&sim, "vsig:motor:phase_current_u"),
        read(&sim, "vsig:motor:phase_current_v"),
        read(&sim, "vsig:motor:phase_current_w"),
    ];
    println!(
        "  after resume: duty_u = {released_duty}, moe = {released_moe}, currents = {currents:?}"
    );
    assert_eq!(
        (released_duty, released_moe),
        (0.0, 0.0),
        "the resumed routes hand the inputs back to the firmware: duty_u = \
         {released_duty}, moe = {released_moe}"
    );
    assert_eq!(
        currents,
        [0.0, 0.0, 0.0],
        "the released legs freewheel to exactly zero: {currents:?}"
    );
}
