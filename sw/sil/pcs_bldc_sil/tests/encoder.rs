//! The AS5048 encoder model standalone (no firmware): signal registration, the unit
//! boundary (`angle[deg]` in, canonical rad stored), quantization + wrap, the
//! pipelined READ-ANGLE wire frame, and the parity-error path. The model is
//! `Cadence::OnDemand` — every observable below is produced by a transfer, never by
//! a scheduled advance; steps in between prove the engine leaves it alone.

use pcs_bldc_sil::board::COUNTS_PER_REV;
use pcs_bldc_sil::{decode_frame, As5048Model, Sil};

#[test]
fn as5048_model_content() {
    const READ_ANGLE: [u8; 2] = [0xFF, 0xFF]; // parity 1, read 1, addr 0x3FFF
    const BAD_PARITY: [u8; 2] = [0x7F, 0xFF]; // 15 ones -> parity invalid
    /// 90 deg, in counts.
    const QUARTER_REV: u16 = (COUNTS_PER_REV / 4.0) as u16;

    let mut sim = Sil::new();
    let motor = sim.add_member(As5048Model::new("as5048_motor", 0.0));
    let dial = sim.add_member(As5048Model::new("dial", 0.0));
    let h_motor = sim
        .link_duplex("spi:test:motor_cs", motor)
        .expect("link the motor encoder");
    let h_dial = sim
        .link_duplex("spi:test:dial_cs", dial)
        .expect("link the dial encoder");

    // One `angle` signal per instance (canonical rad — units are a boundary
    // conversion, never part of the id) plus the raw-ticks output.
    let expected: Vec<String> = ["as5048_motor", "dial"]
        .iter()
        .flat_map(|m| {
            ["angle", "raw_encoder_ticks"]
                .iter()
                .map(move |s| format!("vsig:{m}:{s}"))
        })
        .collect();
    let missing: Vec<&str> = expected
        .iter()
        .filter(|id| !sim.state().signals().any(|s| s.as_str() == **id))
        .map(String::as_str)
        .collect();
    assert!(
        missing.is_empty(),
        "both AS5048 instances register the angle input + raw output; missing: {missing:?}"
    );

    // Command 90 deg through the unit boundary (canonical storage is rad). A step
    // runs no encoder code (OnDemand); two pipelined READ-ANGLE transfers sample
    // the commanded angle at the transaction instant — the response to command N
    // arrives in transfer N+1, so the second frame carries the angle.
    sim.write("vsig:as5048_motor:angle[deg]", 90.0)
        .expect("write angle[deg] = 90");
    sim.step().expect("engine step");
    let first = sim.duplex_transfer(h_motor, &READ_ANGLE).expect("linked bus");
    let second = sim.duplex_transfer(h_motor, &READ_ANGLE).expect("linked bus");
    let decoded = decode_frame(&second);
    assert!(
        matches!(decoded, Some((true, false, QUARTER_REV))),
        "READ-ANGLE response decodes on the wire: frames {first:02X?} then {second:02X?}; second -> {decoded:?}"
    );
    // The transfer also published the quantized (noise-free) raw-ticks output.
    let raw = sim.read_u64("vsig:as5048_motor:raw_encoder_ticks");
    assert_eq!(
        raw,
        u64::from(QUARTER_REV),
        "model quantizes 90 deg -> {QUARTER_REV} counts (a quarter revolution), got {raw}"
    );

    // A negative command wraps into [0, 2pi) — and because the wrapped value differs
    // from the raw command, this also proves the transfer PUBLISHES its folded angle
    // back to the table (the signal is model state, not an echo of the last write).
    sim.write("vsig:as5048_motor:angle[deg]", -90.0)
        .expect("write angle[deg] = -90");
    sim.step().expect("engine step");
    let _ = sim.duplex_transfer(h_motor, &READ_ANGLE).expect("linked bus");
    let wrapped_deg = sim.read_f64("vsig:as5048_motor:angle[deg]");
    let raw_neg = sim.read_u64("vsig:as5048_motor:raw_encoder_ticks");
    let three_quarter_rev = u64::from(QUARTER_REV) * 3;
    assert!(
        ((wrapped_deg - 270.0).abs() < 0.05) && (raw_neg == three_quarter_rev),
        "-90 deg wraps to 270 deg and {three_quarter_rev} counts: angle[deg] reads {wrapped_deg}, raw = {raw_neg}"
    );

    // A parity-corrupt command must surface as the error flag in a later response (on
    // the dial instance, so the motor's state stays clean).
    let _ = sim.duplex_transfer(h_dial, &BAD_PARITY).expect("linked bus");
    let err_resp = sim.duplex_transfer(h_dial, &READ_ANGLE).expect("linked bus");
    let decoded_err = decode_frame(&err_resp);
    assert!(
        matches!(decoded_err, Some((_, true, _))),
        "parity-corrupt command raises the error flag: frame {err_resp:02X?} -> {decoded_err:?}"
    );
}
