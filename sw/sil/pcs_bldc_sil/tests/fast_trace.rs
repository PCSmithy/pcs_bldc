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
    F_SAMPLES, F_TRACE_STATUS,
};
use common::{connected_world, drain_tx, inject, set_u32, u64_at, GRID_US};

/// Protocol address of the sim trace window (`app_server_simTraceWindow32`):
/// word [0] counts 1 ms task passes, word [1] bridge cycle callbacks, and
/// word [2] is word [1]'s complement, written in the same callback.
const WINDOW_BASE: u32 = 0x2000_0000;

/// The cycle-callback duration probe's observed-maximum statics.
const STEP_MAX: &str = "main_cycleProbe_stepMax_us";
const CALLBACK_MAX: &str = "main_cycleProbe_callbackMax_us";

/// The sim USB transfer counter: one per HW_USB_write the board issues.
const TX_WRITES: &str = "HW_USB_sim_data.txWrites";

/// The bounds the cycle callback is held to.
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
// [test->fw~conn_server_006~1]
#[test]
fn one_cycle_and_one_millisecond_groups_stream_together() {
    let mut sim = connected_world(Sil::options().grid_us(GRID_US));
    let mut deframer = Deframer::new();
    let writes_before = u64_at(&sim, TX_WRITES);

    inject(
        &mut sim,
        &watch_request(
            1,
            &[
                (WINDOW_BASE + 4, 4, 1),
                (WINDOW_BASE, 4, 20),
                (WINDOW_BASE + 12, 4, 200),
            ],
        ),
    );
    let envelopes = run(&mut sim, &mut deframer, 600); // 30 ms of PWM periods

    // The one-cycle group: a record every cycle, its counter advancing with it.
    let fast = samples_of(&envelopes, 1);
    assert!(!fast.is_empty(), "the one-cycle group streams");
    // A millisecond's 20 one-cycle records leave as a run of messages that the
    // staggered slow records split: none carries more than a millisecond of
    // them, and the split after the offset-1 record is the one always present.
    let counts: Vec<u32> = fast.iter().map(|batch| batch.count).collect();
    assert!(counts.len() >= 20, "many milliseconds of batches: {counts:?}");
    assert!(
        counts.iter().all(|&count| (count > 0) && (count <= 20)),
        "no message outruns a millisecond of one-cycle records: {counts:?}"
    );
    assert!(
        counts.contains(&2),
        "the 1 ms group's record at offset 1 splits the run after two: {counts:?}"
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
        records.len() >= 580,
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

    // Whatever a pass produced reached the transport as one write — two at
    // most, where a pass outgrew the stage.
    let writes = u64_at(&sim, TX_WRITES) - writes_before;
    assert!(
        writes <= 2 * 30,
        "at most two transfers per 1 ms pass over 30 ms, got {writes}"
    );

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

    // The 10 ms group beside both, one record to a message on its own offset.
    let slowest = samples_of(&envelopes, 200);
    assert!(!slowest.is_empty(), "the 10 ms group streams alongside");
    for (i, batch) in slowest.iter().enumerate() {
        assert_eq!(batch.count, 1, "a 200-cycle record rides its own message");
        assert_eq!(
            batch.cycle_of(0),
            2 + (i as u32 * 200),
            "the 200-cycle group captures at offset 2, every 200 cycles"
        );
    }
}

// [test->sys~obs_005~1]
// [test->fw~conn_trace_002~1]
#[test]
fn one_cycle_watches_admit_on_the_budgets_alone() {
    let mut sim = connected_world(Sil::options().grid_us(GRID_US));
    let mut deframer = Deframer::new();

    // Five 4-byte one-cycle spans: u = 20 x (4 + 20) = 480 B/ms and
    // r = 20 x 20000 + 27 x 1562 = 442174 B/s, both inside the board's budgets.
    let five: Vec<(u32, u32, u32)> = (0..5).map(|i| (WINDOW_BASE + (i * 4), 4, 1)).collect();
    inject(&mut sim, &watch_request(7, &five));
    let envelopes = run(&mut sim, &mut deframer, 200);

    let status = envelopes
        .iter()
        .find(|(id, field, _)| (*id == 7) && (*field == F_TRACE_STATUS))
        .map(|(_, _, bytes)| parse_fields(bytes))
        .expect("TraceStatus reply: nothing caps the one-cycle group's entry count");
    assert_eq!(field_varint(&status, 2), 480, "RAM usage per millisecond");
    assert_eq!(field_varint(&status, 4), 442_174, "link rate");

    let fast = samples_of(&envelopes, 1);
    assert!(!fast.is_empty(), "the five-entry one-cycle group streams");
    assert!(
        fast.iter().all(|batch| batch.record_size() == 20),
        "five 4-byte spans per record"
    );
}

// [test->sys~obs_005~1]
// [test->fw~conn_trace_002~1]
#[test]
fn a_one_cycle_list_over_the_link_budget_is_rejected() {
    let mut sim = connected_world(Sil::options().grid_us(GRID_US));
    let mut deframer = Deframer::new();

    // Five 8-byte one-cycle spans: u = 880 B/ms fits, but
    // r = 40 x 20000 + 27 x 3125 = 884375 B/s is past the 480 kB/s link budget.
    let five: Vec<(u32, u32, u32)> = (0..5).map(|i| (WINDOW_BASE + (i * 8), 8, 1)).collect();
    inject(&mut sim, &watch_request(7, &five));
    let envelopes = run(&mut sim, &mut deframer, 60);

    let verdict = envelopes
        .iter()
        .find(|(id, field, _)| (*id == 7) && (*field == F_RESPONSE))
        .map(|(_, _, bytes)| parse_fields(bytes))
        .expect("Response to the over-budget WatchRequest");
    assert_eq!(field_varint(&verdict, 1), 0, "an over-budget list is rejected");
    let cause = String::from_utf8_lossy(common::proto::field_bytes(&verdict, 2)).to_string();
    assert!(cause.contains("link"), "the cause names the link budget, got {cause:?}");
}

// [test->sys~obs_005~1]
// [test->fw~conn_trace_004~1]
#[test]
fn a_one_cycle_group_captures_the_window_pair_coherently() {
    let mut sim = connected_world(Sil::options().grid_us(GRID_US));
    let mut deframer = Deframer::new();

    // Words [1] and [2] are written together in the cycle callback, [2] the
    // complement of [1] — so a torn capture shows up as a broken complement.
    inject(
        &mut sim,
        &watch_request(1, &[(WINDOW_BASE + 4, 4, 1), (WINDOW_BASE + 8, 4, 1)]),
    );
    let envelopes = run(&mut sim, &mut deframer, 600); // 30 ms of PWM periods

    let fast = samples_of(&envelopes, 1);
    assert!(!fast.is_empty(), "the one-cycle group streams");
    let mut records = 0u32;
    for batch in &fast {
        assert_eq!(batch.record_size(), 8, "two 4-byte spans per record");
        for k in 0..batch.count {
            let bytes = batch.record(k);
            let counter = u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]);
            let complement = u32::from_le_bytes([bytes[4], bytes[5], bytes[6], bytes[7]]);
            // The sim runs one fiber, so this proves the capture is coherent
            // across cycles, not that it is atomic against a preempting ISR.
            assert_eq!(
                complement,
                !counter,
                "cycle {} is a torn snapshot: {counter:#x} / {complement:#x}",
                batch.cycle_of(k)
            );
            records += 1;
        }
    }
    assert!(
        records >= 580,
        "every PWM period carries a coherent record: {records} over 600"
    );
}

// [test->fw~conn_trace_002~1]
// [test->fw~conn_trace_004~1]
#[test]
fn a_live_list_swap_restarts_the_stream_at_the_new_width() {
    let mut sim = connected_world(Sil::options().grid_us(GRID_US));
    let mut deframer = Deframer::new();

    inject(&mut sim, &watch_request(1, &[(WINDOW_BASE + 4, 4, 1)]));
    let first = run(&mut sim, &mut deframer, 200);
    let before = samples_of(&first, 1);
    assert!(!before.is_empty(), "the first list streams");
    assert!(
        before.iter().all(|batch| batch.record_size() == 4),
        "one 4-byte span per record before the swap"
    );

    // A second list installs over the first, with nothing stopped in between.
    inject(
        &mut sim,
        &watch_request(2, &[(WINDOW_BASE + 4, 4, 1), (WINDOW_BASE + 8, 4, 1)]),
    );
    let second = run(&mut sim, &mut deframer, 200);

    // The TraceStatus reply marks the install: everything after it is the new
    // list, since the install discards the prior list's buffered records.
    let swap_at = second
        .iter()
        .position(|(id, field, _)| (*id == 2) && (*field == F_TRACE_STATUS))
        .expect("TraceStatus reply to the second WatchRequest");
    let after = samples_of(&second[swap_at..], 1);
    assert!(!after.is_empty(), "the second list streams");

    let mut cycles: Vec<u32> = Vec::new();
    for batch in &after {
        assert_eq!(
            batch.record_size(),
            8,
            "the new span's width rides every record after the swap"
        );
        for k in 0..batch.count {
            let bytes = batch.record(k);
            let counter = u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]);
            let complement = u32::from_le_bytes([bytes[4], bytes[5], bytes[6], bytes[7]]);
            assert_eq!(complement, !counter, "the new list's spans are captured together");
            cycles.push(batch.cycle_of(k));
        }
    }
    assert_eq!(
        cycles[0], 0,
        "the swap restarts the cycle index at the group's offset"
    );
    for (i, &cycle) in cycles.iter().enumerate() {
        assert_eq!(cycle, i as u32, "contiguous cycle indices after the swap");
    }
}

// [test->sys~obs_005~1]
// [test->fw~conn_trace_004~1] a stall costs one contiguous gap: once a record
// is skipped nothing is admitted until the buffer has drained to half, and the
// records that follow form one gap-free run
#[test]
fn a_stalled_transport_costs_one_gap_then_streams_clean() {
    let mut sim = connected_world(Sil::options().grid_us(GRID_US));
    let mut deframer = Deframer::new();

    inject(&mut sim, &watch_request(1, &[(WINDOW_BASE + 4, 4, 1)]));
    let mut envelopes = run(&mut sim, &mut deframer, 200);

    // The host stops reading: transmit capacity fills, emission stops, and the
    // sample ring overflows.
    for _ in 0..800 {
        sim.step().expect("engine step");
    }

    // Reading resumes: the backlog leaves and admission re-opens at half.
    envelopes.extend(run(&mut sim, &mut deframer, 1200));

    let mut cycles: Vec<u32> = Vec::new();
    for batch in samples_of(&envelopes, 1) {
        for k in 0..batch.count {
            cycles.push(batch.cycle_of(k));
        }
    }
    let gaps: Vec<(u32, u32)> = cycles
        .windows(2)
        .filter(|pair| pair[1] != pair[0] + 1)
        .map(|pair| (pair[0], pair[1]))
        .collect();
    assert_eq!(gaps.len(), 1, "a stall costs exactly one cycle-index gap: {gaps:?}");

    let resume_at = gaps[0].1;
    let tail: Vec<u32> = cycles.iter().copied().filter(|&c| c >= resume_at).collect();
    for (i, &cycle) in tail.iter().enumerate() {
        assert_eq!(cycle, resume_at + (i as u32), "the stream resumes gap-free");
    }
    assert!(
        tail.len() >= 1000,
        "the resumed run streams every PWM period: {} records over 1200",
        tail.len()
    );
}

// [test->fw~mc_018~1] the probe mechanics: a standing maximum holds, a zero
// write clears it, and the following callbacks re-arm it (the duration bound
// itself is bench-verified)
#[test]
fn the_cycle_callback_probe_clears_and_re_arms() {
    let mut sim = connected_world(Sil::options().grid_us(GRID_US));
    for _ in 0..400 {
        sim.step().expect("engine step");
    }

    // The sim clock advances once per grid step, so a duration taken inside one
    // callback quantizes to zero: these read as the probe's arm state, and the
    // budgets below are fw~mc_018's contract, not a measurement.
    let step_max = u64_at(&sim, STEP_MAX);
    let callback_max = u64_at(&sim, CALLBACK_MAX);
    assert_eq!(step_max, 0, "the empty commutation step measures zero");
    assert_eq!(callback_max, 0, "an intra-callback duration quantizes to zero");
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
    assert_eq!(
        rearmed, 0,
        "the cleared maximum restarts from the following callbacks"
    );
}
