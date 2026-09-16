//! The signal-trace services over the framed protocol, end to end: a
//! WatchRequest installed through the sim USB RX path streams the sim trace
//! window's 1 kHz counter back as coherent Samples on the 1 ms group; the trace
//! capability report carries the board's budgets; a written span reads back.
//!
//! The world runs on the control-rate grid, so one engine step is one PWM
//! period and one bridge cycle callback — the cadence the sampler counts in.

use pcs_bldc_sil::{wire::Deframer, Sil};

mod common;
use common::proto::{
    field_bytes, field_varint, parse_envelope, parse_fields, read_request, samples, watch_request,
    write_request, Samples, F_READ_REPLY, F_RESPONSE, F_SAMPLES, F_TRACE_STATUS,
};
use common::{connected_world, drain_tx, inject, GRID_US};

/// Protocol address of the sim trace window (`app_server_simTraceWindow32`);
/// word [0] is the 1 kHz counter task_1ms increments. Words [1] and [2] are the
/// cycle callback's, so the one-shot round-trip below uses word [3], which no
/// firmware writer touches.
const WINDOW_BASE: u32 = 0x2000_0000;

/// Step the world, draining the sim USB capture each step into `deframer`, and
/// return every envelope that completed.
fn run(sim: &mut Sil, deframer: &mut Deframer, steps: u32) -> Vec<(u64, u32, Vec<u8>)> {
    let mut envelopes = Vec::new();
    for _ in 0..steps {
        sim.step().expect("engine step");
        let capture = drain_tx(sim);
        for payload in deframer.push(&capture) {
            if let Some(env) = parse_envelope(&payload) {
                envelopes.push(env);
            }
        }
    }
    envelopes
}

fn samples_of(envelopes: &[(u64, u32, Vec<u8>)]) -> Vec<Samples> {
    envelopes
        .iter()
        .filter(|(_, field, _)| *field == F_SAMPLES)
        .map(|(_, _, bytes)| samples(bytes))
        .collect()
}

// [test->sys~obs_005~1]
// [test->sys~obs_008~1]
// [test->sys~obs_009~1]
// [test->fw~conn_trace_002~1]
// [test->fw~conn_trace_004~1]
// [test->fw~conn_trace_006~1]
// [test->fw~conn_trace_007~1]
#[test]
fn trace_stream() {
    let mut sim = connected_world(Sil::options().grid_us(GRID_US));
    let mut deframer = Deframer::new();

    // One 4-byte watch on the 1 kHz counter word, at the 20-cycle (1 ms) period.
    inject(&mut sim, &watch_request(1, &[(WINDOW_BASE, 4, 20)]));
    let envelopes = run(&mut sim, &mut deframer, 1200); // 60 ms of PWM periods

    // The accepted WatchRequest answers with the trace capability report,
    // carrying the board's configured budgets and the list's usage:
    // u = 1 x (4 + 4) B/ms, r = 4 B x 1000 Hz + 27 B x 1000 msg/s.
    let status = envelopes
        .iter()
        .find(|(id, field, _)| (*id == 1) && (*field == F_TRACE_STATUS))
        .map(|(_, _, bytes)| parse_fields(bytes))
        .expect("TraceStatus reply to the accepted WatchRequest");
    assert_eq!(field_varint(&status, 1), 2048, "sample-RAM budget, bytes per ms");
    assert_eq!(field_varint(&status, 2), 4 + 4, "RAM usage per millisecond");
    assert_eq!(field_varint(&status, 3), 480_000, "link budget");
    assert_eq!(field_varint(&status, 4), 31_000, "link rate");

    // The counter arrives as coherent records: cycle indices on the group's
    // offset and period, the watched value advancing once per millisecond.
    let batches = samples_of(&envelopes);
    assert!(!batches.is_empty(), "the 1 ms group streams");
    let mut records: Vec<(u32, u32)> = Vec::new();
    for batch in &batches {
        assert_eq!(batch.period_cycles, 20, "the group's period rides the message");
        for k in 0..batch.count {
            let bytes = batch.record(k);
            let value = u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]);
            records.push((batch.cycle_of(k), value));
        }
    }
    assert!(
        records.len() >= 58,
        "a 1 ms watch streams every 20 cycles: {} records over 60 ms",
        records.len()
    );
    let (c0, v0) = records[0];
    assert_eq!(c0, 1, "the 20-cycle group's first record sits on its offset");
    for (i, &(cycle, value)) in records.iter().enumerate() {
        assert_eq!(cycle, c0 + (i as u32 * 20), "records every 20 cycles");
        assert_eq!(
            u64::from(value),
            u64::from(v0) + i as u64,
            "counter values consecutive and coherent with the cycle index"
        );
    }

    // One-shot write then read back: the span's current contents round-trip.
    inject(&mut sim, &write_request(2, WINDOW_BASE + 12, &[0xDE, 0xAD, 0xBE, 0xEF]));
    let mut envelopes = run(&mut sim, &mut deframer, 60);
    inject(&mut sim, &read_request(3, WINDOW_BASE + 12, 4));
    envelopes.extend(run(&mut sim, &mut deframer, 60));

    let write_reply = envelopes
        .iter()
        .find(|(id, field, _)| (*id == 2) && (*field == F_RESPONSE))
        .map(|(_, _, bytes)| parse_fields(bytes))
        .expect("Response to the WriteRequest");
    assert_eq!(field_varint(&write_reply, 1), 1, "write accepted");
    let read_reply = envelopes
        .iter()
        .find(|(id, field, _)| (*id == 3) && (*field == F_READ_REPLY))
        .map(|(_, _, bytes)| parse_fields(bytes))
        .expect("ReadReply to the ReadRequest");
    assert_eq!(
        field_bytes(&read_reply, 1),
        &[0xDE, 0xAD, 0xBE, 0xEF],
        "the read returns the span's current contents"
    );
}
