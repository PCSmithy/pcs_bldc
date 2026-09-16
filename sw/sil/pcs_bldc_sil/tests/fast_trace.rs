//! The one-cycle trace path: a watch at the PWM period streams a record every
//! bridge cycle callback, batched into Samples messages beside a 1 ms watch on
//! its own stagger offset — and the callback's duration probe reads back and
//! re-arms when the host clears it.
//!
//! One engine step is one PWM period on this grid, so the sim ADC's injected
//! completion dispatches the cycle callback once per step.

use pcs_bldc_sil::{wire::Deframer, Sil};

mod common;
use common::proto::{
    field_varint, parse_envelope, parse_fields, samples, watch_request, Samples, F_RESPONSE,
    F_SAMPLES,
};
use common::{connected_world, drain_tx, inject, set_u32, u64_at, GRID_US};

/// Protocol address of the sim trace window (`app_server_simTraceWindow32`):
/// word [0] counts 1 ms task passes, word [1] bridge cycle callbacks.
const WINDOW_BASE: u32 = 0x2000_0000;

/// The observed-maximum statics of fw~mc_018.
const STEP_MAX: &str = "main_cycleProbe_stepMax_us";
const CALLBACK_MAX: &str = "main_cycleProbe_callbackMax_us";

/// The bounds sys~mc_006 holds the cycle callback to.
const STEP_BUDGET_US: u64 = 20;
const CALLBACK_BUDGET_US: u64 = 40;

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

fn samples_of(envelopes: &[(u64, u32, Vec<u8>)], period: u32) -> Vec<Samples> {
    envelopes
        .iter()
        .filter(|(_, field, _)| *field == F_SAMPLES)
        .map(|(_, _, bytes)| samples(bytes))
        .filter(|batch| batch.period_cycles == period)
        .collect()
}

// [test->sys~obs_005~1]
// [test->fw~conn_trace_002~1]
// [test->fw~conn_trace_004~1]
// [test->fw~conn_trace_009~1]
#[test]
fn one_cycle_and_one_millisecond_groups_stream_together() {
    let mut sim = connected_world(Sil::options().grid_us(GRID_US));
    let mut deframer = Deframer::new();

    inject(
        &mut sim,
        &watch_request(1, &[(WINDOW_BASE + 4, 4, 1), (WINDOW_BASE, 4, 20)]),
    );
    let envelopes = run(&mut sim, &mut deframer, 600); // 30 ms of PWM periods

    // The one-cycle group: a record every cycle, its counter advancing with it.
    let fast = samples_of(&envelopes, 1);
    assert!(!fast.is_empty(), "the one-cycle group streams");
    assert!(
        fast.iter().any(|batch| batch.count > 1),
        "consecutive records of the group share a message"
    );
    let mut records: Vec<(u32, u32)> = Vec::new();
    for batch in &fast {
        for k in 0..batch.count {
            let bytes = batch.record(k);
            records.push((
                batch.cycle_of(k),
                u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]),
            ));
        }
    }
    assert!(
        records.len() >= 400,
        "a one-cycle watch streams every PWM period: {} records over 600",
        records.len()
    );
    let (c0, v0) = records[0];
    assert_eq!(c0, 0, "the one-cycle group's offset is zero");
    for (i, &(cycle, value)) in records.iter().enumerate() {
        assert_eq!(cycle, c0 + i as u32, "consecutive cycle indices");
        assert_eq!(
            u64::from(value),
            u64::from(v0) + i as u64,
            "the per-cycle counter advances once per record"
        );
    }

    // The 1 ms group beside it, on its own stagger offset.
    let medium = samples_of(&envelopes, 20);
    assert!(!medium.is_empty(), "the 1 ms group streams alongside");
    let mut slow_cycles: Vec<u32> = Vec::new();
    for batch in &medium {
        for k in 0..batch.count {
            slow_cycles.push(batch.cycle_of(k));
        }
    }
    for (i, &cycle) in slow_cycles.iter().enumerate() {
        assert_eq!(cycle % 20, 1, "the 20-cycle group captures at offset 1");
        assert_eq!(cycle, 1 + (i as u32 * 20), "one record every 20 cycles");
    }
}

// [test->sys~obs_005~1]
// [test->fw~conn_trace_002~1]
#[test]
fn a_fifth_one_cycle_watch_is_rejected() {
    let mut sim = connected_world(Sil::options().grid_us(GRID_US));
    let mut deframer = Deframer::new();

    let five: Vec<(u32, u32, u32)> = (0..5).map(|i| (WINDOW_BASE + (i * 4), 4, 1)).collect();
    inject(&mut sim, &watch_request(7, &five));
    let envelopes = run(&mut sim, &mut deframer, 60);

    let verdict = envelopes
        .iter()
        .find(|(id, field, _)| (*id == 7) && (*field == F_RESPONSE))
        .map(|(_, _, bytes)| parse_fields(bytes))
        .expect("Response to the over-capacity WatchRequest");
    assert_eq!(field_varint(&verdict, 1), 0, "a fifth one-cycle watch is rejected");
    let cause = String::from_utf8_lossy(common::proto::field_bytes(&verdict, 2)).to_string();
    assert!(cause.contains("one-cycle"), "the cause names the cap, got {cause:?}");

    // Four is the cap, not a rejection.
    inject(&mut sim, &watch_request(8, &five[..4]));
    let envelopes = run(&mut sim, &mut deframer, 60);
    assert!(
        envelopes
            .iter()
            .any(|(id, field, _)| (*id == 8) && (*field == 31)),
        "four one-cycle watches admit, answering with TraceStatus"
    );
}

// [test->fw~mc_018~1]
#[test]
fn the_cycle_callback_probe_reads_back_and_re_arms() {
    let mut sim = connected_world(Sil::options().grid_us(GRID_US));
    for _ in 0..400 {
        sim.step().expect("engine step");
    }

    // The maxima hold what the callback actually cost. The sim clock advances
    // once per grid step, so an intra-callback duration quantizes to zero — the
    // bounds below are the contract, not a measurement of the real firmware.
    let step_max = u64_at(&sim, STEP_MAX);
    let callback_max = u64_at(&sim, CALLBACK_MAX);
    assert!(step_max <= STEP_BUDGET_US, "step maximum {step_max} us");
    assert!(callback_max <= CALLBACK_BUDGET_US, "callback maximum {callback_max} us");

    // A maximum only ever rises: a sentinel above every observed duration
    // stands until the host clears it.
    set_u32(&sim, CALLBACK_MAX, 9_999);
    for _ in 0..200 {
        sim.step().expect("engine step");
    }
    assert_eq!(
        u64_at(&sim, CALLBACK_MAX),
        9_999,
        "the probe never lowers a standing maximum"
    );

    // Writing zero clears it, and the next callbacks re-arm it from scratch.
    set_u32(&sim, CALLBACK_MAX, 0);
    for _ in 0..200 {
        sim.step().expect("engine step");
    }
    let rearmed = u64_at(&sim, CALLBACK_MAX);
    assert!(
        rearmed <= CALLBACK_BUDGET_US,
        "the cleared maximum restarts from the following callbacks, got {rearmed} us"
    );
}
