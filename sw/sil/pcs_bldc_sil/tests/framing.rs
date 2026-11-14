//! Message framing at the system level (sys~conn_002): bytes corrupted or deleted
//! mid-stream, in either direction, lose only the frames that carried them. Host
//! to board, the firmware's deframer discards the damaged requests and answers
//! the intact one after them; board to host, the app's deframer (`pcs_wire`)
//! discards the damaged frames of a captured stream and decodes every other.

mod common;

use common::proto::{decode, envelope, ping, Payload};
use common::{connected_world, inject, inject_wire, Host};
use pcs_bldc_sil::wire::{frame, Deframer};
use pcs_bldc_sil::{Sil, SilOptions};

/// One byte inside a frame's COBS body changed to another nonzero byte, so the
/// delimiters stand and only the CRC-32 (or the COBS structure) can catch it.
fn corrupt(segment: &mut [u8], at: usize) {
    let b = segment[at];
    segment[at] = if (b ^ 0x55) != 0 { b ^ 0x55 } else { b ^ 0xAA };
}

/// The non-telemetry replies over `ms` of run, as (request_id, payload).
fn replies_over(sim: &mut Sil, ms: u32) -> Vec<(u32, Payload)> {
    let mut host = Host::new();
    let mut replies = Vec::new();
    for _ in 0..ms {
        sim.run_for_ms(1);
        for env in host.poll(sim) {
            if let Some(payload) = env.payload {
                if !matches!(payload, Payload::Telemetry(_)) {
                    replies.push((env.request_id, payload));
                }
            }
        }
    }
    replies
}

// [test->sys~conn_002~1]
#[test]
fn damaged_requests_are_discarded_and_the_next_is_answered() {
    let mut sim = connected_world(SilOptions::default());
    let mut corrupted = frame(&envelope(1, ping()));
    let mid = corrupted.len() / 2;
    corrupt(&mut corrupted, mid);
    let mut truncated = frame(&envelope(2, ping()));
    truncated.remove(truncated.len() / 2);
    let mut wire = corrupted;
    wire.extend(truncated);
    wire.extend(frame(&envelope(3, ping())));
    inject_wire(&mut sim, &wire);

    let replies = replies_over(&mut sim, 20);
    println!("framing: host -> board, replies to {:?}", replies.iter().map(|(id, _)| *id).collect::<Vec<_>>());
    assert_eq!(replies.len(), 1, "the two damaged frames get no reply: {replies:?}");
    assert_eq!(replies[0].0, 3, "the intact frame behind them is answered");

    // The link carries on: the next request is answered too.
    inject(&mut sim, &envelope(4, ping()));
    let replies = replies_over(&mut sim, 20);
    assert_eq!(replies.iter().map(|(id, _)| *id).collect::<Vec<_>>(), vec![4], "the next request is answered");
}

// [test->sys~conn_002~1]
#[test]
fn damaged_frames_from_the_board_cost_only_themselves() {
    // A captured stream of the board's frames, a byte corrupted in one and a byte
    // deleted from another: the host deframer yields every other frame unchanged.
    let mut sim = connected_world(SilOptions::default());
    let mut wire = Vec::new();
    for id in 10..16u32 {
        inject(&mut sim, &envelope(id, ping()));
        sim.run_for_ms(20);
        wire.extend(pcs_bldc_sil::wire::read_tx_capture(&sim.fw()));
        let _ = common::drain_tx(&mut sim);
    }
    let segments: Vec<Vec<u8>> = wire.split(|&b| b == 0).filter(|s| !s.is_empty()).map(<[u8]>::to_vec).collect();
    let clean: Vec<Vec<u8>> = Deframer::new().push(&wire);
    assert_eq!(clean.len(), segments.len(), "every captured segment is a valid frame");
    assert!(clean.len() >= 6, "the capture holds the replies and the telemetry: {} frames", clean.len());

    let (hit, cut) = (1usize, clean.len() - 2);
    let mut damaged = Vec::new();
    for (k, segment) in segments.iter().enumerate() {
        let mut segment = segment.clone();
        if k == hit {
            let mid = segment.len() / 2;
            corrupt(&mut segment, mid);
        }
        if k == cut {
            segment.remove(segment.len() / 2);
        }
        damaged.push(0);
        damaged.extend(segment);
    }
    damaged.push(0);
    let received = Deframer::new().push(&damaged);
    let expected: Vec<Vec<u8>> =
        clean.iter().enumerate().filter(|(k, _)| (*k != hit) && (*k != cut)).map(|(_, p)| p.clone()).collect();
    println!("framing: board -> host, {} frames captured, {} decoded after damaging two", clean.len(), received.len());
    assert_eq!(received, expected, "only the two damaged frames are lost");
    assert!(received.iter().all(|p| decode(p).is_some()), "every surviving frame decodes as an Envelope");
}
