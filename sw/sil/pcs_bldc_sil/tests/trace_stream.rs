//! The signal-trace services over the framed protocol, end to end: a
//! WatchRequest installed through the sim USB RX path streams the sim trace
//! window's 1 kHz counter back as coherent Samples on the 1 ms group; the trace
//! capability report carries the board's budgets; a written span reads back.
//!
//! The world runs on the control-rate grid, so one engine step is one PWM
//! period and one bridge cycle callback — the cadence the sampler counts in.

use pcs_bldc_sil::Sil;

mod common;
use common::proto::{read_request, trace::Samples, watch_request, write_request, Envelope, Payload, SamplesExt};
use common::{accepted, connected_world, inject, reply_in, Host, GRID_US};

/// Protocol address of the sim trace window (`app_server_simTraceWindow32`);
/// word [0] is the 1 kHz counter task_1ms increments. Words [1] and [2] are the
/// cycle callback's, so the one-shot round-trip below uses word [3], which no
/// firmware writer touches.
const WINDOW_BASE: u32 = 0x2000_0000;

fn samples_of(envelopes: &[Envelope]) -> Vec<Samples> {
    envelopes
        .iter()
        .filter_map(|env| match &env.payload {
            Some(Payload::Samples(batch)) => Some(batch.clone()),
            _ => None,
        })
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
    let mut host = Host::new();

    // One 4-byte watch on the 1 kHz counter word, at the 20-cycle (1 ms) period.
    inject(&mut sim, &watch_request(1, &[(WINDOW_BASE, 4, 20)]));
    let envelopes = host.run_steps(&mut sim, 1200); // 60 ms of PWM periods

    // The accepted WatchRequest answers with the trace capability report,
    // carrying the board's configured budgets and the list's usage:
    // u = 1 x (4 + 4) B/ms, r = 4 B x 1000 Hz + 27 B x 1000 msg/s.
    let Some(Payload::TraceStatus(status)) = reply_in(&envelopes, 1) else {
        panic!("TraceStatus reply to the accepted WatchRequest");
    };
    assert_eq!(status.ram_budget_bytes_per_ms, 2048, "sample-RAM budget, bytes per ms");
    assert_eq!(status.ram_usage_bytes_per_ms, 4 + 4, "RAM usage per millisecond");
    assert_eq!(status.link_budget_bytes_per_s, 480_000, "link budget");
    assert_eq!(status.link_rate_bytes_per_s, 31_000, "link rate");

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
        // The counter is 1 kHz and the group's period is 1 ms, so the value a
        // record carries follows its own cycle index, not its place in the run.
        assert_eq!(
            u64::from(value) - u64::from(v0),
            u64::from((cycle - c0) / 20),
            "counter values coherent with the cycle index"
        );
    }

    // One-shot write then read back: the span's current contents round-trip.
    inject(&mut sim, &write_request(2, WINDOW_BASE + 12, &[0xDE, 0xAD, 0xBE, 0xEF]));
    let mut envelopes = host.run_steps(&mut sim, 60);
    inject(&mut sim, &read_request(3, WINDOW_BASE + 12, 4));
    envelopes.extend(host.run_steps(&mut sim, 60));

    // [test->fw~conn_trace_008~1]
    let write_reply = reply_in(&envelopes, 2).expect("Response to the WriteRequest");
    assert!(accepted(write_reply), "write accepted");
    let Some(Payload::ReadReply(read_reply)) = reply_in(&envelopes, 3) else {
        panic!("ReadReply to the ReadRequest");
    };
    assert_eq!(
        read_reply.data,
        [0xDE, 0xAD, 0xBE, 0xEF],
        "the read returns the span's current contents"
    );
}
