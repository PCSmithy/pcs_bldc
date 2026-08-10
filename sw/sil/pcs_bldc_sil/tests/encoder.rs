//! The AS5048 encoder model standalone (no firmware): signal registration, the unit
//! boundary (`angle[deg]` in, canonical rad stored), quantization + wrap, the
//! pipelined READ-ANGLE wire frame, and the parity-error path.

use pcs_bldc_sil::board::COUNTS_PER_REV;
use pcs_bldc_sil::{decode_frame, As5048Model, Sil};
use voyant::DuplexPeer;

#[test]
fn as5048_model_content() {
    const READ_ANGLE: [u8; 2] = [0xFF, 0xFF]; // parity 1, read 1, addr 0x3FFF
    const BAD_PARITY: [u8; 2] = [0x7F, 0xFF]; // 15 ones -> parity invalid
    /// 90 deg, in counts.
    const QUARTER_REV: u16 = (COUNTS_PER_REV / 4.0) as u16;

    let mut sim = Sil::new();
    let motor = sim.add_member(As5048Model::new("as5048_motor", 0.0));
    let dial = sim.add_member(As5048Model::new("dial", 0.0));

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

    // Command 90 deg through the unit boundary (canonical storage is rad); one step
    // folds it into the model and publishes the quantized output.
    sim.write("vsig:as5048_motor:angle[deg]", 90.0)
        .expect("write angle[deg] = 90");
    sim.step().expect("engine step");
    let raw = sim.read_u64("vsig:as5048_motor:raw_encoder_ticks");
    assert_eq!(
        raw,
        u64::from(QUARTER_REV),
        "model quantizes 90 deg -> {QUARTER_REV} counts (a quarter revolution), got {raw}"
    );

    // Two pipelined READ-ANGLE transfers: the response to command N arrives in
    // transfer N+1, so the second frame carries the angle.
    let (first, second) = {
        let mut m = motor.borrow_mut();
        (m.transfer(&READ_ANGLE), m.transfer(&READ_ANGLE))
    };
    let decoded = decode_frame(&second);
    assert!(
        matches!(decoded, Some((true, false, QUARTER_REV))),
        "READ-ANGLE response decodes on the wire: frames {first:02X?} then {second:02X?}; second -> {decoded:?}"
    );

    // A negative command wraps into [0, 2pi) — and because the wrapped value differs
    // from the raw command, this also proves the model PUBLISHES its folded angle back
    // to the table (the signal is model state, not an echo of the last write).
    sim.write("vsig:as5048_motor:angle[deg]", -90.0)
        .expect("write angle[deg] = -90");
    sim.step().expect("engine step");
    let wrapped_deg = sim.read_f64("vsig:as5048_motor:angle[deg]");
    let raw_neg = sim.read_u64("vsig:as5048_motor:raw_encoder_ticks");
    let three_quarter_rev = u64::from(QUARTER_REV) * 3;
    assert!(
        ((wrapped_deg - 270.0).abs() < 0.05) && (raw_neg == three_quarter_rev),
        "-90 deg wraps to 270 deg and {three_quarter_rev} counts: angle[deg] reads          {wrapped_deg}, raw = {raw_neg}"
    );

    // A parity-corrupt command must surface as the error flag in a later response (on
    // the dial instance, so the motor's state stays clean).
    let err_resp = {
        let mut d = dial.borrow_mut();
        let _ = d.transfer(&BAD_PARITY);
        d.transfer(&READ_ANGLE)
    };
    let decoded_err = decode_frame(&err_resp);
    assert!(
        matches!(decoded_err, Some((_, true, _))),
        "parity-corrupt command raises the error flag: frame {err_resp:02X?} -> {decoded_err:?}"
    );
}
