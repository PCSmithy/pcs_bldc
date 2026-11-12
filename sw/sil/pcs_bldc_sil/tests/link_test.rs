//! The link throughput test over the framed protocol, end to end: a
//! LinkTestRequest injected through the sim USB RX path is accepted, and the
//! board streams exactly the requested frames with consecutive sequence
//! numbers and the patterned payload the host verifies content against.
//!
//! The sim USB TX buffer never drains on its own, so the capture is read and
//! zeroed every step — the same trick trace_stream.rs uses, here feeding one
//! incremental Deframer so a frame split across that boundary still rejoins.
//! Envelope encoding, RX injection and TX draining all come from `common`.

use pcs_bldc_sil::Sil;

mod common;
use common::proto::{link_test_request, Envelope, Payload};
use common::{accepted, connected_world, inject, reply_in, Host};

fn is_link_test_frame(env: &Envelope) -> bool {
    matches!(env.payload, Some(Payload::LinkTestFrame(_)))
}

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

    let mut host = Host::new();
    let mut envelopes: Vec<Envelope> = Vec::new();
    for _ in 0..MAX_STEPS {
        envelopes.extend(host.run_steps(&mut sim, 1));
        if envelopes.iter().filter(|env| is_link_test_frame(env)).count() as u32 >= FRAME_COUNT {
            break;
        }
    }

    // The request is accepted before a single frame streams.
    let response = reply_in(&envelopes, 1).expect("Response to the LinkTestRequest");
    assert!(accepted(response), "link test accepted");

    // Exactly the requested frames, seq 0..FRAME_COUNT-1, each carrying the
    // (seq + i) & 0xFF pattern — a host verifies content without a side channel.
    let streamed: Vec<_> = envelopes.iter().filter(|env| is_link_test_frame(env)).collect();
    assert_eq!(
        streamed.len() as u32,
        FRAME_COUNT,
        "exactly the requested frame count streams"
    );
    for (i, env) in streamed.iter().enumerate() {
        assert_eq!(env.request_id, 0, "frames stream unsolicited, like Samples");
        let Some(Payload::LinkTestFrame(frame)) = &env.payload else { unreachable!() };
        assert_eq!(frame.seq, i as u32, "consecutive sequence numbers from zero");
        assert_eq!(frame.payload.len() as u32, PAYLOAD_BYTES, "requested payload size");
        let expected: Vec<u8> = (0..PAYLOAD_BYTES)
            .map(|i| (frame.seq.wrapping_add(i) & 0xFF) as u8)
            .collect();
        assert_eq!(frame.payload, expected, "seq-derived payload pattern");
    }

    // The count is reached, so the service is free: nothing more streams.
    let after = host.run_steps(&mut sim, 5);
    assert!(!after.iter().any(is_link_test_frame), "the stream stops at the count");
}
