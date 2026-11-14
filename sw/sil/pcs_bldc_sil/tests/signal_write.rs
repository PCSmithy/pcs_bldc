//! Signal write at the system level (sys~obs_006): host WriteRequests land on an
//! 8-byte variable the firmware's trace sampler reads every millisecond, and the
//! trace of that variable carries every written value, each record the prior
//! value or the written one in full.

mod common;

use common::proto::{trace::Samples, watch_request, write_request, Envelope, Payload, SamplesExt};
use common::{accepted, connected_world, inject, reply_in, Host, GRID_US};
use pcs_bldc_sil::Sil;

/// Protocol address of the sim trace window (`app_server_simTraceWindow32`);
/// words [4] and [5] are scratch no firmware writer touches.
const WINDOW_BASE: u32 = 0x2000_0000;
const VARIABLE: u32 = WINDOW_BASE + 16;
const SIZE: usize = 8;

/// The 1 ms watch period, in 50 us cycles.
const PERIOD_CYCLES: u32 = 20;

/// Writes made, each 3 ms after the last.
const WRITES: u8 = 20;

fn samples_of(envelopes: &[Envelope]) -> Vec<Samples> {
    envelopes
        .iter()
        .filter_map(|env| match &env.payload {
            Some(Payload::Samples(batch)) => Some(batch.clone()),
            _ => None,
        })
        .collect()
}

// [test->sys~obs_006~1]
#[test]
fn every_traced_read_of_a_written_variable_is_whole() {
    // Value k is eight bytes of k, so a read straddling a write would show two
    // byte values in one record.
    let mut sim = connected_world(Sil::options().grid_us(GRID_US));
    let mut host = Host::new();
    inject(&mut sim, &watch_request(1, &[(VARIABLE, SIZE as u32, PERIOD_CYCLES)]));
    let mut envelopes = host.run_steps(&mut sim, 100);
    assert!(reply_in(&envelopes, 1).is_some(), "the watch is installed");
    for k in 1..=WRITES {
        inject(&mut sim, &write_request(100 + u32::from(k), VARIABLE, &[k; SIZE]));
        envelopes.extend(host.run_steps(&mut sim, 60));
    }
    envelopes.extend(host.run_steps(&mut sim, 100));

    for k in 1..=WRITES {
        let reply = reply_in(&envelopes, 100 + u32::from(k)).expect("a Response to each write");
        assert!(accepted(reply), "write {k} is accepted");
    }
    let mut records = Vec::new();
    for batch in samples_of(&envelopes) {
        for r in 0..batch.count {
            records.push(batch.record(r).to_vec());
        }
    }
    let mut last = 0u8;
    let mut seen = Vec::new();
    for record in &records {
        assert_eq!(record.len(), SIZE, "one 8-byte variable per record");
        assert!(record.iter().all(|&b| b == record[0]), "a whole value, never a mix: {record:02X?}");
        assert!((record[0] == last) || (record[0] == last + 1), "the prior or the written value: {last} then {}", record[0]);
        if record[0] != last {
            seen.push(record[0]);
        }
        last = record[0];
    }
    println!("signal_write: {} traced reads of the variable over {WRITES} writes, every one whole", records.len());
    assert_eq!(seen, (1..=WRITES).collect::<Vec<_>>(), "the trace returns every written value in turn");
    assert_eq!(last, WRITES, "the trace returns the last written value");
}
