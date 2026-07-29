//! The AS5048 encoder model standalone (no firmware): signal registration, the unit
//! boundary (`angle[deg]` in, canonical rad stored), quantization + wrap, the
//! pipelined READ-ANGLE wire frame, and the parity-error path.

use pcs_bldc_sil::{As5048Model, Sil};
use voyant::{DuplexPeer, Value};

/// Decode an AS5048 wire frame (big-endian: byte 0 = bits 15..8) into
/// `(parity_ok, error_flag, raw14)`. Even parity: the count of ones across all 16
/// bits (parity bit included) must be even.
fn as5048_decode(frame: &[u8]) -> Option<(bool, bool, u16)> {
    if frame.len() != 2 {
        return None;
    }
    let f = u16::from_be_bytes([frame[0], frame[1]]);
    Some((f.count_ones().is_multiple_of(2), (f & 0x4000) != 0, f & 0x3FFF))
}

#[test]
fn as5048_model_content() {
    const READ_ANGLE: [u8; 2] = [0xFF, 0xFF]; // parity 1, read 1, addr 0x3FFF
    const BAD_PARITY: [u8; 2] = [0x7F, 0xFF]; // 15 ones -> parity invalid

    let mut sim = Sil::new();
    let motor = sim.add_member(As5048Model::new("as5048_motor", 0.0));
    let dial = sim.add_member(As5048Model::new("dial", 0.0));

    // One `angle` signal per instance (canonical rad — units are a boundary
    // conversion, never part of the id) plus the raw-ticks output.
    let expected: Vec<String> = ["as5048_motor", "dial"]
        .iter()
        .flat_map(|m| ["angle", "raw_encoder_ticks"].iter().map(move |s| format!("vsig:{m}:{s}")))
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
    sim.write("vsig:as5048_motor:angle[deg]", 90.0).expect("write angle[deg] = 90");
    sim.step().expect("engine step");
    let raw = sim
        .read("vsig:as5048_motor:raw_encoder_ticks")
        .ok()
        .flatten()
        .as_ref()
        .and_then(Value::as_u64);
    assert_eq!(
        raw,
        Some(4096),
        "model quantizes 90 deg -> 4096 counts (16384/4), got {raw:?}"
    );

    // Two pipelined READ-ANGLE transfers: the response to command N arrives in
    // transfer N+1, so the second frame carries the angle.
    let (first, second) = {
        let mut m = motor.borrow_mut();
        (m.transfer(&READ_ANGLE), m.transfer(&READ_ANGLE))
    };
    let decoded = as5048_decode(&second);
    assert!(
        matches!(decoded, Some((true, false, 4096))),
        "READ-ANGLE response decodes on the wire: frames {first:02X?} then {second:02X?}; second -> {decoded:?}"
    );

    // A negative command wraps into [0, 2pi) — and because the wrapped value differs
    // from the raw command, this also proves the model PUBLISHES its folded angle back
    // to the table (the signal is model state, not an echo of the last write).
    sim.write("vsig:as5048_motor:angle[deg]", -90.0).expect("write angle[deg] = -90");
    sim.step().expect("engine step");
    let wrapped_deg = sim
        .read("vsig:as5048_motor:angle[deg]")
        .ok()
        .flatten()
        .as_ref()
        .and_then(Value::as_f64);
    let raw_neg = sim
        .read("vsig:as5048_motor:raw_encoder_ticks")
        .ok()
        .flatten()
        .as_ref()
        .and_then(Value::as_u64);
    assert!(
        matches!(wrapped_deg, Some(d) if (d - 270.0).abs() < 0.05) && (raw_neg == Some(12288)),
        "-90 deg wraps to 270 deg and 12288 counts: angle[deg] reads {wrapped_deg:?}, raw = {raw_neg:?}"
    );

    // A parity-corrupt command must surface as the error flag in a later response (on
    // the dial instance, so the motor's state stays clean).
    let err_resp = {
        let mut d = dial.borrow_mut();
        let _ = d.transfer(&BAD_PARITY);
        d.transfer(&READ_ANGLE)
    };
    let decoded_err = as5048_decode(&err_resp);
    assert!(
        matches!(decoded_err, Some((_, true, _))),
        "parity-corrupt command raises the error flag: frame {err_resp:02X?} -> {decoded_err:?}"
    );
}
