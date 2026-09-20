//! The link throughput test over the framed protocol, end to end: a
//! LinkTestRequest injected through the sim USB RX path is accepted, and the
//! board streams exactly the requested frames with consecutive sequence
//! numbers and the patterned payload the host verifies content against.
//!
//! The sim USB TX buffer never drains on its own, so the capture is read and
//! zeroed every step — the same trick trace_stream.rs uses, here feeding one
//! incremental Deframer so a frame split across that boundary still rejoins.
//! Envelope encoding, RX injection and TX draining all come from `common`.

use pcs_bldc_sil::{wire::Deframer, Sil};

mod common;
use common::proto::{
    field_bytes, field_varint, link_test_request, parse_envelope, parse_fields, F_LINK_TEST_FRAME,
    F_RESPONSE,
};
use common::{connected_world, drain_tx, inject};

/// The measurement this scenario asks for.
const PAYLOAD_BYTES: u32 = 64;
const FRAME_COUNT: u32 = 200;

/// Step budget for the stream. The sim transport holds 2 KB, so a step carries
/// ~26 frames of this size; the margin absorbs the telemetry sharing the link.
const MAX_STEPS: u32 = 120;

// [test->sys~conn_004~1]
// [test->fw~conn_server_005~1]
#[test]
fn link_test() {
    let mut sim = connected_world(Sil::options());

    inject(&mut sim, &link_test_request(1, PAYLOAD_BYTES, FRAME_COUNT));

    let mut deframer = Deframer::new();
    let mut envelopes: Vec<(u64, u32, Vec<u8>)> = Vec::new();
    let mut frames = 0u32;
    for _ in 0..MAX_STEPS {
        sim.step().expect("engine step");
        let capture = drain_tx(&mut sim);
        for payload in deframer.push(&capture) {
            if let Some(env) = parse_envelope(&payload) {
                if env.1 == F_LINK_TEST_FRAME {
                    frames += 1;
                }
                envelopes.push(env);
            }
        }
        if frames >= FRAME_COUNT {
            break;
        }
    }

    // The request is accepted before a single frame streams.
    let response = envelopes
        .iter()
        .find(|(id, field, _)| (*id == 1) && (*field == F_RESPONSE))
        .map(|(_, _, bytes)| parse_fields(bytes))
        .expect("Response to the LinkTestRequest");
    assert_eq!(field_varint(&response, 1), 1, "link test accepted");

    // Exactly the requested frames, seq 0..FRAME_COUNT-1, each carrying the
    // (seq + i) & 0xFF pattern — a host verifies content without a side channel.
    let streamed: Vec<_> = envelopes
        .iter()
        .filter(|(_, field, _)| *field == F_LINK_TEST_FRAME)
        .collect();
    assert_eq!(
        streamed.len() as u32,
        FRAME_COUNT,
        "exactly the requested frame count streams"
    );
    for (i, (request_id, _, bytes)) in streamed.iter().enumerate() {
        assert_eq!(*request_id, 0, "frames stream unsolicited, like Samples");
        let fields = parse_fields(bytes);
        let seq = field_varint(&fields, 1);
        assert_eq!(seq, i as u64, "consecutive sequence numbers from zero");
        let payload = field_bytes(&fields, 2);
        assert_eq!(payload.len() as u32, PAYLOAD_BYTES, "requested payload size");
        let expected: Vec<u8> = (0..PAYLOAD_BYTES)
            .map(|i| ((seq as u32).wrapping_add(i) & 0xFF) as u8)
            .collect();
        assert_eq!(payload, expected.as_slice(), "seq-derived payload pattern");
    }

    // The count is reached, so the service is free: nothing more streams.
    for _ in 0..5 {
        sim.step().expect("engine step");
        let capture = drain_tx(&mut sim);
        for payload in deframer.push(&capture) {
            if let Some((_, field, _)) = parse_envelope(&payload) {
                assert_ne!(field, F_LINK_TEST_FRAME, "the stream stops at the count");
            }
        }
    }
}
