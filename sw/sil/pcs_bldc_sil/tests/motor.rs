//! Motor scaffold: the plant→encoder→firmware chain carries data before any physics
//! exists. A [`MotorModel`] holds a stub shaft angle; a zero-latency route feeds that
//! angle into the AS5048 encoder model, which quantizes it the way real hardware does.
//! Proves the outputs are unit-registered, the firmware's PWM ports are readable (dark
//! at boot), and the angle route closes motor→encoder→firmware — all on stub state.

use pcs_bldc_sil::{As5048Model, MotorModel, Sil, SOURCE};
use voyant::{vsig_id, SignalId, Value};

#[test]
fn motor_scaffold_closes_the_loop() {
    const STUB_ANGLE_RAD: f64 = 0.75;
    const COUNTS_PER_REV: f64 = 16384.0;
    let two_pi = 2.0 * std::f64::consts::PI;

    let mut sim = Sil::new();
    // Producer → consumer → firmware: motor before encoder before firmware, so the
    // zero-latency angle route lands in the encoder the same tick the motor records it.
    sim.add_member(MotorModel::new("motor", STUB_ANGLE_RAD as f32));
    let encoder = sim.add_member(As5048Model::new("as5048_motor", 0.0));
    let fwm = sim.load_firmware(SOURCE);
    sim.add_member(fwm);

    // Close the loop: the motor's shaft angle drives the encoder model's input.
    sim.add_route(
        vsig_id("motor", "angle").expect("valid vsig id"),
        vsig_id("as5048_motor", "angle").expect("valid vsig id"),
    )
    .expect("route motor angle into the encoder");
    sim.link_duplex("spi:pcs_bldc:AS5048_1", encoder).expect("link the encoder to the firmware SPI");

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
        "PWM_U_duty", "PWM_V_duty", "PWM_W_duty",
        "PWM_U_enabled", "PWM_V_enabled", "PWM_W_enabled", "TIM1_MOE",
    ];
    let non_zero: Vec<String> = pwm_ports
        .iter()
        .filter_map(|p| {
            let id = format!("vsig:{SOURCE}:{p}");
            match sim.read(&id).ok().flatten().as_ref().and_then(Value::as_f64) {
                Some(0.0) => None,
                other => Some(format!("{p}={other:?}")),
            }
        })
        .collect();
    assert!(non_zero.is_empty(), "PWM observation ports read 0.0 (dark bridge); non-zero: {non_zero:?}");

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
    assert_eq!(raw, expected_raw, "angle route carries the stub angle into the encoder");

    // The firmware polled the encoder over the real SPI bus — the loop's last hop.
    let rx_id = SignalId::parse("spi:pcs_bldc:AS5048_1:rx").expect("valid spi id");
    assert!(
        sim.state().changes(&rx_id).map(|c| !c.is_empty()).unwrap_or(false),
        "firmware ran SPI transfers against the encoder model"
    );
}
