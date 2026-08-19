//! Byte-exact wire contract: prost's encoding must equal the hand-rolled
//! minimal-varint encodings the firmware accepted on real hardware (the SIL's
//! trace_stream.rs oracle), and the envelope's pinned field numbers must hold
//! (framework 2..29, trace 30..59, board 60..62).

use prost::Message;

// --- the hand-rolled oracle (ported from sw/sil/pcs_bldc_sil/tests/trace_stream.rs) ---

fn put_varint(mut v: u64, out: &mut Vec<u8>) {
    loop {
        let byte = (v & 0x7F) as u8;
        v >>= 7;
        if v != 0 {
            out.push(byte | 0x80);
        } else {
            out.push(byte);
            break;
        }
    }
}

fn put_len_key(field: u32, out: &mut Vec<u8>) {
    put_varint(u64::from((field << 3) | 2), out);
}

fn envelope(request_id: u64, payload_field: u32, payload: &[u8]) -> Vec<u8> {
    let mut env = Vec::new();
    if request_id != 0 {
        env.push(0x08);
        put_varint(request_id, &mut env);
    }
    put_len_key(payload_field, &mut env);
    put_varint(payload.len() as u64, &mut env);
    env.extend_from_slice(payload);
    env
}

fn watch_request(request_id: u64, watches: &[(u32, u32, u32)]) -> Vec<u8> {
    let mut wr = Vec::new();
    for &(address, size, period_ms) in watches {
        let mut w = Vec::new();
        w.push(0x08);
        put_varint(u64::from(address), &mut w);
        w.push(0x10);
        put_varint(u64::from(size), &mut w);
        w.push(0x18);
        put_varint(u64::from(period_ms), &mut w);
        wr.push(0x0A);
        put_varint(w.len() as u64, &mut wr);
        wr.extend_from_slice(&w);
    }
    envelope(request_id, 30, &wr)
}

fn prost_envelope(request_id: u32, payload: pcs_proto::shared::envelope::Payload) -> Vec<u8> {
    pcs_proto::shared::Envelope {
        request_id,
        payload: Some(payload),
    }
    .encode_to_vec()
}

#[test]
fn watch_request_matches_hand_rolled_bytes() {
    let watches = vec![
        pcs_proto::trace::Watch { address: 0x2000_0000, size: 4, period_ms: 1 },
        pcs_proto::trace::Watch { address: 0x2000_0010, size: 2, period_ms: 10 },
    ];
    let bytes = prost_envelope(
        7,
        pcs_proto::shared::envelope::Payload::WatchRequest(pcs_proto::trace::WatchRequest {
            watches,
        }),
    );
    let oracle = watch_request(7, &[(0x2000_0000, 4, 1), (0x2000_0010, 2, 10)]);
    assert_eq!(bytes, oracle);
}

#[test]
fn stream_envelope_omits_zero_request_id() {
    // An unsolicited stream envelope (request_id 0) must start at its oneof
    // tag: Samples is Envelope field 33 -> key (33 << 3) | 2 = 0x8A 0x02.
    let bytes = prost_envelope(
        0,
        pcs_proto::shared::envelope::Payload::Samples(pcs_proto::trace::Samples {
            tick_ms: 0,
            data: vec![0xAB],
        }),
    );
    assert_eq!(&bytes[..2], &[0x8A, 0x02]);
}

#[test]
fn telemetry_tag_matches_wire_constant() {
    // board.Telemetry is Envelope field 62; pcs_wire's capture filter keys on
    // its leading tag bytes.
    let bytes = prost_envelope(
        0,
        pcs_proto::shared::envelope::Payload::Telemetry(pcs_proto::board::Telemetry::default()),
    );
    assert_eq!(&bytes[..2], &pcs_wire::TELEMETRY_TAG_BYTES);
}

#[test]
fn response_round_trips_through_frame_codec() {
    // Payload codec + frame codec composed: encode, frame, deframe, decode.
    let sent = prost_envelope(
        9,
        pcs_proto::shared::envelope::Payload::Response(pcs_proto::shared::Response {
            accepted: false,
            cause: "watch span not readable".into(),
        }),
    );
    let wire = pcs_wire::frame(&sent);
    let frames = pcs_wire::parse_frames(&wire);
    assert_eq!(frames.len(), 1);
    let decoded = pcs_proto::shared::Envelope::decode(frames[0].as_slice()).unwrap();
    assert_eq!(decoded.request_id, 9);
    match decoded.payload {
        Some(pcs_proto::shared::envelope::Payload::Response(r)) => {
            assert!(!r.accepted);
            assert_eq!(r.cause, "watch span not readable");
        }
        other => panic!("wrong payload arm: {other:?}"),
    }
}
