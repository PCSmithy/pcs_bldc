//! The trace client: watch installation behind the identity gate, `Samples`
//! demultiplexing of each group's batched records, and batched "samples"
//! events toward the webview (the UI gets ~20 Hz). Cycle indices convert to
//! the webview's millisecond domain here — one PWM cycle is 0.05 ms.

use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use pcs_proto::shared::envelope::Payload;
use pcs_proto::trace::TraceStatus;
use tauri::{AppHandle, Emitter, State};

use crate::firmware::{identity_matches, FirmwareState};
use crate::protocol::Client;
use crate::session::{SamplesConsumer, SessionState};

/// Batched samples flush thresholds: whichever trips first. The point cap
/// bounds one event's payload when a device stall's backlog arrives at once
/// (a one-cycle group delivers 20 000 records per second per entry).
const BATCH_EMIT_INTERVAL: Duration = Duration::from_millis(50);
const BATCH_EMIT_MAX_POINTS: usize = 16_384;

/// PWM cycles per millisecond: the wire's cycle index over the webview's
/// millisecond tick domain.
const CYCLES_PER_MS: f64 = 20.0;

#[derive(serde::Deserialize)]
pub struct WatchSpec {
    pub path: String,
    pub period_cycles: u32,
}

#[derive(Clone)]
struct WatchEntry {
    path: String,
    size: u32,
    period_cycles: u32,
    leaf: dwarf_map::Leaf,
}

struct WatchTable {
    entries: Vec<WatchEntry>,
}

#[derive(Clone, serde::Serialize)]
struct SignalSeries {
    path: String,
    points: Vec<(f64, f64)>,
}

#[derive(Clone, serde::Serialize)]
pub struct SamplesBatch {
    signals: Vec<SignalSeries>,
    dropped_records: u32,
}

/// Little-endian typed decode to the plot currency. Enums decode as their
/// unsigned integer value at their byte size.
fn decode(leaf: dwarf_map::Leaf, bytes: &[u8]) -> f64 {
    use dwarf_map::{Leaf, Scalar};
    let unsigned = |b: &[u8]| -> u64 {
        b.iter()
            .enumerate()
            .fold(0u64, |v, (i, &x)| v | (u64::from(x) << (8 * i)))
    };
    match leaf {
        Leaf::Enum(_) => unsigned(bytes) as f64,
        Leaf::Scalar(kind) => match kind {
            Scalar::U8 | Scalar::U16 | Scalar::U32 | Scalar::U64 => unsigned(bytes) as f64,
            Scalar::Bool => f64::from(bytes[0] != 0),
            Scalar::I8 => f64::from(bytes[0] as i8),
            Scalar::I16 => f64::from(i16::from_le_bytes([bytes[0], bytes[1]])),
            Scalar::I32 => f64::from(i32::from_le_bytes(bytes[..4].try_into().unwrap())),
            Scalar::I64 => i64::from_le_bytes(bytes[..8].try_into().unwrap()) as f64,
            Scalar::F32 => f64::from(f32::from_le_bytes(bytes[..4].try_into().unwrap())),
            Scalar::F64 => f64::from_le_bytes(bytes[..8].try_into().unwrap()),
        },
    }
}

/// Demultiplex one `Samples` message: its data is `count` consecutive
/// records of the entries whose period is the message's, each record the
/// group's bytes in watch-list order, record k captured at cycle index
/// `first_cycle + k * period_cycles`. A length that is not `count` whole
/// records means a corrupt or foreign message — dropped whole.
// [impl->app~obs_004~1]
fn demux(table: &WatchTable, samples: &pcs_proto::trace::Samples) -> Vec<(usize, u32, f64)> {
    let group: Vec<(usize, &WatchEntry)> = table
        .entries
        .iter()
        .enumerate()
        .filter(|(_, e)| e.period_cycles == samples.period_cycles)
        .collect();
    let record: usize = group.iter().map(|(_, e)| e.size as usize).sum();
    let count = samples.count as usize;
    if (record == 0) || (count == 0) || (record * count != samples.data.len()) {
        return Vec::new();
    }
    let mut out = Vec::with_capacity(group.len() * count);
    let mut offset = 0usize;
    for k in 0..count {
        let cycle = samples
            .first_cycle
            .wrapping_add((k as u32).wrapping_mul(samples.period_cycles));
        for (i, entry) in &group {
            let bytes = &samples.data[offset..offset + entry.size as usize];
            offset += entry.size as usize;
            out.push((*i, cycle, decode(entry.leaf, bytes)));
        }
    }
    out
}

/// The consumer-side accumulator: demuxed points per entry, flushed as one
/// "samples" event when a threshold trips.
struct BatchState {
    table: WatchTable,
    buffers: Vec<Vec<(f64, f64)>>,
    /// Newest cycle index seen per entry — groups run at their own rates, so
    /// a missed record is only visible against its own entry's last one.
    prev_cycle: Vec<Option<u32>>,
    dropped_records: u32,
    points_since_emit: usize,
    last_emit: Instant,
}

impl BatchState {
    fn new(table: WatchTable) -> Self {
        let buffers = table.entries.iter().map(|_| Vec::new()).collect();
        let prev_cycle = table.entries.iter().map(|_| None).collect();
        Self {
            table,
            buffers,
            prev_cycle,
            dropped_records: 0,
            points_since_emit: 0,
            last_emit: Instant::now(),
        }
    }

    fn ingest(&mut self, samples: &pcs_proto::trace::Samples) -> Option<SamplesBatch> {
        let points = demux(&self.table, samples);
        if points.is_empty() {
            return None;
        }
        self.points_since_emit += points.len();
        for (entry, cycle, value) in points {
            let period = self.table.entries[entry].period_cycles;
            if let Some(prev) = self.prev_cycle[entry] {
                let delta = cycle.wrapping_sub(prev);
                // Past half the u32 range the index went backwards: the
                // counter wrapped (~59.65 h of uptime) or the stream re-armed.
                // Resync on the new domain instead of charging the whole span
                // as dropped records.
                if (delta <= (u32::MAX / 2)) && (delta > period) {
                    self.dropped_records += (delta / period) - 1;
                }
            }
            self.prev_cycle[entry] = Some(cycle);
            self.buffers[entry].push((f64::from(cycle) / CYCLES_PER_MS, value));
        }
        if (self.points_since_emit >= BATCH_EMIT_MAX_POINTS)
            || (self.last_emit.elapsed() >= BATCH_EMIT_INTERVAL)
        {
            return Some(self.flush());
        }
        None
    }

    fn flush(&mut self) -> SamplesBatch {
        let signals = self
            .table
            .entries
            .iter()
            .zip(self.buffers.iter_mut())
            .filter(|(_, buf)| !buf.is_empty())
            .map(|(entry, buf)| SignalSeries {
                path: entry.path.clone(),
                points: std::mem::take(buf),
            })
            .collect();
        let batch = SamplesBatch {
            signals,
            dropped_records: self.dropped_records,
        };
        self.dropped_records = 0;
        self.points_since_emit = 0;
        self.last_emit = Instant::now();
        batch
    }
}

#[derive(Default)]
pub struct TraceState(Mutex<Option<Arc<Mutex<BatchState>>>>);

/// Drop any accumulator without emitting — a dead session's residual points
/// must not leak into the next connection.
pub fn drop_accumulator(trace: &TraceState) {
    trace
        .0
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
        .take();
}

/// Flush any prior accumulator through `emit` (residual points would
/// otherwise vanish on a list change) and uninstall it.
fn flush_prior(trace: &TraceState, emit: &dyn Fn(SamplesBatch)) {
    let prior = trace
        .0
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
        .take();
    if let Some(prior) = prior {
        let batch = prior
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .flush();
        if !batch.signals.is_empty() {
            emit(batch);
        }
    }
}

/// Unwrap a watch/status reply into the device's `TraceStatus`, or the
/// rejection cause.
fn expect_trace_status(reply: Payload) -> Result<TraceStatus, String> {
    match reply {
        Payload::TraceStatus(ts) => Ok(ts),
        Payload::Response(r) => Err(if r.cause.is_empty() {
            "watch list rejected".to_string()
        } else {
            r.cause
        }),
        other => Err(format!("unexpected reply: {other:?}")),
    }
}

/// The connected session's client handle and reported build id (lock scope:
/// session only; requests run unlocked).
fn session_client(session: &State<SessionState>) -> Result<(Arc<Client>, String), String> {
    let guard = session.0.lock().map_err(|_| "session state poisoned")?;
    let session = guard.as_ref().ok_or("not connected")?;
    Ok((
        session.client.clone(),
        session.device_build_id().to_string(),
    ))
}

/// Send the watch list and, on acceptance, commit the replacement table and
/// build its consumer. A rejection returns the cause and leaves any prior
/// table untouched (the prior list keeps streaming, mirroring the
/// firmware's admission).
// [impl->app~obs_003~1]
fn perform_install(
    client: &Client,
    trace: &TraceState,
    wire_watches: Vec<pcs_proto::trace::Watch>,
    entries: Vec<WatchEntry>,
    emit: Box<dyn Fn(SamplesBatch) + Send + Sync>,
) -> Result<(TraceStatus, Option<SamplesConsumer>), String> {
    let reply = client.request(Payload::WatchRequest(pcs_proto::trace::WatchRequest {
        watches: wire_watches,
    }))?;
    let status = expect_trace_status(reply)?;

    flush_prior(trace, emit.as_ref());
    let consumer = if entries.is_empty() {
        // An accepted empty list stops the stream: nothing to install.
        None
    } else {
        let shared = Arc::new(Mutex::new(BatchState::new(WatchTable { entries })));
        let consumer_shared = shared.clone();
        let consumer: SamplesConsumer = Arc::new(move |samples| {
            let batch = consumer_shared
                .lock()
                .ok()
                .and_then(|mut state| state.ingest(&samples));
            if let Some(batch) = batch {
                emit(batch);
            }
        });
        // The device already accepted: the table commit must never be lost.
        *trace
            .0
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner) = Some(shared);
        Some(consumer)
    };
    Ok((status, consumer))
}

// [impl->app~obs_002~1]
// [impl->app~obs_003~1]
#[tauri::command]
pub fn install_watches(
    app: AppHandle,
    session: State<SessionState>,
    firmware: State<FirmwareState>,
    trace: State<TraceState>,
    watches: Vec<WatchSpec>,
) -> Result<TraceStatus, String> {
    for w in &watches {
        if !matches!(w.period_cycles, 1 | 20 | 200) {
            return Err(format!(
                "{}: period {} cycles is not 1/20/200",
                w.path, w.period_cycles
            ));
        }
    }

    // Resolve against the loaded ELF (lock scope: firmware only).
    let (elf_build_id, wire_watches, entries) = {
        let guard = firmware.0.lock().map_err(|_| "firmware state poisoned")?;
        let loaded = guard.as_ref().ok_or("no firmware ELF loaded")?;
        let mut wire = Vec::with_capacity(watches.len());
        let mut entries = Vec::with_capacity(watches.len());
        for w in &watches {
            let (address, size, leaf) = loaded.resolve_watch(&w.path)?;
            wire.push(pcs_proto::trace::Watch {
                address,
                size,
                period_cycles: w.period_cycles,
            });
            entries.push(WatchEntry {
                path: w.path.clone(),
                size,
                period_cycles: w.period_cycles,
                leaf,
            });
        }
        (loaded.build_id.clone(), wire, entries)
    };

    let (client, device_build_id) = session_client(&session)?;

    if !identity_matches(&device_build_id, &elf_build_id) {
        return Err(format!(
            "identity mismatch: device reports {device_build_id}, loaded ELF is \
             {elf_build_id} — reflash the board or load the matching ELF"
        ));
    }

    let emit_app = app.clone();
    let (status, consumer) = perform_install(
        &client,
        &trace,
        wire_watches,
        entries,
        Box::new(move |batch| {
            let _ = emit_app.emit("samples", batch);
        }),
    )?;

    install_consumer(&session, consumer);
    let _ = app.emit("trace-status", status);
    Ok(status)
}

/// Post-acceptance consumer swap: the device is already serving the new
/// list, so a poisoned session lock must not strand the stream.
fn install_consumer(session: &State<SessionState>, consumer: Option<SamplesConsumer>) {
    let guard = session
        .0
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    if let Some(session) = guard.as_ref() {
        session.set_samples_consumer(consumer);
    }
}

/// Clear = install the empty list: teardown happens only after the device
/// confirms (a timed-out clear leaves the prior list streaming, mirroring
/// the install path).
#[tauri::command]
pub fn clear_watches(
    app: AppHandle,
    session: State<SessionState>,
    trace: State<TraceState>,
) -> Result<TraceStatus, String> {
    let (client, _) = session_client(&session)?;
    let emit_app = app.clone();
    let (status, consumer) = perform_install(
        &client,
        &trace,
        Vec::new(),
        Vec::new(),
        Box::new(move |batch| {
            let _ = emit_app.emit("samples", batch);
        }),
    )?;
    install_consumer(&session, consumer);
    let _ = app.emit("trace-status", status);
    Ok(status)
}

#[tauri::command]
pub fn trace_status(session: State<SessionState>) -> Result<TraceStatus, String> {
    let (client, _) = session_client(&session)?;
    expect_trace_status(client.request(Payload::TraceStatusRequest(
        pcs_proto::trace::TraceStatusRequest::default(),
    ))?)
}

#[cfg(test)]
mod tests {
    use super::*;
    use dwarf_map::{Leaf, Scalar};

    fn entry(path: &str, size: u32, period_cycles: u32, leaf: Leaf) -> WatchEntry {
        WatchEntry {
            path: path.into(),
            size,
            period_cycles,
            leaf,
        }
    }

    /// One group's message: `count` records of the `period_cycles` entries,
    /// the first captured at `first_cycle`.
    fn samples(
        first_cycle: u32,
        period_cycles: u32,
        count: u32,
        data: &[u8],
    ) -> pcs_proto::trace::Samples {
        pcs_proto::trace::Samples {
            first_cycle,
            data: data.to_vec(),
            period_cycles,
            count,
        }
    }

    fn mixed_table() -> WatchTable {
        WatchTable {
            entries: vec![
                entry("a", 4, 1, Leaf::Scalar(Scalar::U32)),
                entry("b", 2, 20, Leaf::Scalar(Scalar::U16)),
                entry("c", 1, 200, Leaf::Scalar(Scalar::U8)),
                entry("d", 1, 20, Leaf::Scalar(Scalar::U8)),
            ],
        }
    }

    // [test->app~obs_004~1]
    #[test]
    fn a_message_maps_to_its_own_group_in_list_order() {
        let table = mixed_table();
        // The 20-cycle group is entries b and d, 3 bytes per record.
        assert_eq!(
            demux(&table, &samples(21, 20, 1, &[0xEF, 0xBE, 0x7F])),
            vec![(1, 21, f64::from(0xBEEFu16)), (3, 21, f64::from(0x7Fu8))]
        );
        // The one-cycle group is entry a alone.
        assert_eq!(
            demux(&table, &samples(4, 1, 1, &[1, 0, 0, 0])),
            vec![(0, 4, 1.0)]
        );
        // The 200-cycle group is entry c alone.
        assert_eq!(
            demux(&table, &samples(202, 200, 1, &[9])),
            vec![(2, 202, 9.0)]
        );
    }

    // [test->app~obs_004~1]
    #[test]
    fn batched_records_land_at_cycle_indices_spaced_by_the_period() {
        let table = mixed_table();
        // Three one-cycle records from cycle 7: consecutive cycle indices.
        assert_eq!(
            demux(
                &table,
                &samples(7, 1, 3, &[1, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0])
            ),
            vec![(0, 7, 1.0), (0, 8, 2.0), (0, 9, 3.0)]
        );
        // Two 20-cycle records from cycle 21: b|d then b|d, 20 cycles apart.
        assert_eq!(
            demux(&table, &samples(21, 20, 2, &[1, 0, 10, 2, 0, 20])),
            vec![(1, 21, 1.0), (3, 21, 10.0), (1, 41, 2.0), (3, 41, 20.0)]
        );
    }

    // [test->app~obs_004~1]
    #[test]
    fn a_cycle_index_gap_yields_values_at_exactly_the_received_indices() {
        let mut state = BatchState::new(WatchTable {
            entries: vec![
                entry("fast", 1, 1, Leaf::Scalar(Scalar::U8)),
                entry("slow", 1, 200, Leaf::Scalar(Scalar::U8)),
            ],
        });
        // Re-arm the interval clock so a stalled test host cannot trigger an
        // early flush and steal points from the final message.
        for (first, data) in [(0u32, [0u8, 1]), (5, [5, 6])] {
            state.last_emit = Instant::now();
            let _ = state.ingest(&samples(first, 1, 2, &data));
        }
        // The slow group's own first record: its gap counter starts here, not
        // against the fast group's indices.
        state.last_emit = Instant::now();
        let _ = state.ingest(&samples(2, 200, 1, &[7]));
        let batch = state.flush();
        let fast = &batch.signals[0];
        assert_eq!(fast.path, "fast");
        assert_eq!(
            fast.points,
            vec![(0.0, 0.0), (0.05, 1.0), (0.25, 5.0), (0.30, 6.0)]
        );
        assert_eq!(batch.signals[1].points, vec![(0.1, 7.0)]);
        // Cycles 2, 3, 4 never arrived: counted as dropped, not synthesized.
        assert_eq!(batch.dropped_records, 3);
    }

    // [test->app~obs_004~1]
    #[test]
    fn a_backwards_cycle_index_resyncs_instead_of_counting_drops() {
        let mut state = BatchState::new(WatchTable {
            entries: vec![entry("fast", 1, 1, Leaf::Scalar(Scalar::U8))],
        });
        state.last_emit = Instant::now();
        let _ = state.ingest(&samples(1_000_000, 1, 2, &[1, 2]));
        // The u32 index wrapped and the domain restarted near 0: resync on it
        // rather than charge the backwards span as billions of drops.
        state.last_emit = Instant::now();
        let _ = state.ingest(&samples(0, 1, 2, &[3, 4]));
        let batch = state.flush();
        assert_eq!(batch.dropped_records, 0);
        assert_eq!(state.prev_cycle[0], Some(1));
        assert_eq!(batch.signals[0].points.len(), 4);
    }

    // [test->app~obs_004~1]
    #[test]
    fn every_scalar_kind_decodes() {
        let cases: Vec<(Leaf, Vec<u8>, f64)> = vec![
            (Leaf::Scalar(Scalar::U8), vec![0xFF], 255.0),
            (Leaf::Scalar(Scalar::I8), vec![0x80], -128.0),
            (
                Leaf::Scalar(Scalar::U16),
                vec![0x34, 0x12],
                f64::from(0x1234u16),
            ),
            (Leaf::Scalar(Scalar::I16), vec![0xFE, 0xFF], -2.0),
            (
                Leaf::Scalar(Scalar::U32),
                vec![1, 0, 0, 0x80],
                f64::from(0x8000_0001u32),
            ),
            (
                Leaf::Scalar(Scalar::I32),
                vec![0xFF, 0xFF, 0xFF, 0xFF],
                -1.0,
            ),
            (
                Leaf::Scalar(Scalar::I64),
                (-3i64).to_le_bytes().to_vec(),
                -3.0,
            ),
            (Leaf::Scalar(Scalar::U64), 7u64.to_le_bytes().to_vec(), 7.0),
            (
                Leaf::Scalar(Scalar::F32),
                (-1.5f32).to_le_bytes().to_vec(),
                -1.5,
            ),
            (
                Leaf::Scalar(Scalar::F64),
                (6.25f64).to_le_bytes().to_vec(),
                6.25,
            ),
            (Leaf::Scalar(Scalar::Bool), vec![2], 1.0),
            (Leaf::Enum(42), vec![3, 0, 0, 0], 3.0),
            // The enum wire contract: bytes zero-extend, never sign-extend —
            // firmware.rs's enumerator wrap relies on this domain.
            (Leaf::Enum(42), vec![0xFF], 255.0),
        ];
        for (leaf, bytes, expected) in cases {
            let table = WatchTable {
                entries: vec![entry("x", bytes.len() as u32, 1, leaf)],
            };
            let got = demux(&table, &samples(0, 1, 1, &bytes));
            assert_eq!(got.len(), 1, "{leaf:?}");
            assert_eq!(got[0].2, expected, "{leaf:?}");
        }
    }

    // [test->app~obs_004~1]
    #[test]
    fn length_mismatch_drops_the_whole_message() {
        let table = mixed_table();
        // The 20-cycle group's record is 3 bytes: 2 records need 6.
        assert!(demux(&table, &samples(21, 20, 2, &[0; 5])).is_empty());
        assert!(demux(&table, &samples(21, 20, 2, &[0; 7])).is_empty());
        // A period no entry holds has no membership at all.
        assert!(demux(&table, &samples(0, 2, 1, &[0; 4])).is_empty());
        // And a valid message right after still demuxes.
        assert_eq!(demux(&table, &samples(4, 1, 1, &[9, 0, 0, 0])).len(), 1);
    }

    #[test]
    fn batch_emits_at_the_point_threshold() {
        let mut state = BatchState::new(WatchTable {
            entries: vec![entry("a", 1, 1, Leaf::Scalar(Scalar::U8))],
        });
        // Re-arm the interval clock each message so only the point threshold
        // can trip, however slowly the test host runs. 64 records a message is
        // the shape a one-cycle group at 20 kHz actually delivers.
        const PER_MSG: u32 = 64;
        let data = [0u8; PER_MSG as usize];
        let mut emitted = None;
        let mut first = 0u32;
        while emitted.is_none() {
            state.last_emit = Instant::now();
            emitted = state.ingest(&samples(first, 1, PER_MSG, &data));
            first += PER_MSG;
        }
        let batch = emitted.expect("threshold emit");
        assert_eq!(first as usize, BATCH_EMIT_MAX_POINTS);
        assert_eq!(batch.signals[0].points.len(), BATCH_EMIT_MAX_POINTS);
        assert_eq!(state.points_since_emit, 0);
    }

    /// Drive `perform_install` against a mock wire that answers the watch
    /// request with `reply`, and return the outcome plus the trace state.
    fn run_install(
        reply: Payload,
    ) -> (
        Result<(TraceStatus, Option<SamplesConsumer>), String>,
        TraceState,
    ) {
        use crate::testutil::{wait_for_requests, SharedBuf};
        use prost::Message;

        let buf = SharedBuf::default();
        let client = Client::new(Box::new(buf.clone()));
        let mut pump = client.pump(Box::new(|_| {}));
        let trace = TraceState::default();
        // Pre-install a prior table the outcome is judged against.
        *trace.0.lock().unwrap() = Some(Arc::new(Mutex::new(BatchState::new(WatchTable {
            entries: vec![entry("prior", 1, 1, Leaf::Scalar(Scalar::U8))],
        }))));

        let mut result = None;
        std::thread::scope(|scope| {
            let installer = scope.spawn(|| {
                perform_install(
                    &client,
                    &trace,
                    vec![pcs_proto::trace::Watch {
                        address: 0x2000_0000,
                        size: 4,
                        period_cycles: 1,
                    }],
                    vec![entry("new", 4, 1, Leaf::Scalar(Scalar::U32))],
                    Box::new(|_| {}),
                )
            });
            // Answer the request once it hits the mock wire.
            let request_id = wait_for_requests(&buf, 1)[0];
            let env = pcs_proto::shared::Envelope {
                request_id,
                payload: Some(match &reply {
                    Payload::TraceStatus(ts) => Payload::TraceStatus(*ts),
                    Payload::Response(r) => Payload::Response(r.clone()),
                    _ => unreachable!(),
                }),
            };
            pump.push(&pcs_wire::frame(&env.encode_to_vec()));
            result = Some(installer.join().unwrap());
        });
        (result.unwrap(), trace)
    }

    /// A rejected install leaves the prior table intact (the prior list keeps
    /// streaming); an accepted one replaces it and yields a consumer.
    // [test->app~obs_003~1]
    #[test]
    fn rejected_install_leaves_prior_state_intact() {
        let (result, trace) = run_install(Payload::Response(pcs_proto::shared::Response {
            accepted: false,
            cause: "exceeds link budget".into(),
        }));
        match result {
            Err(cause) => assert_eq!(cause, "exceeds link budget"),
            Ok(_) => panic!("rejection was accepted"),
        }
        let guard = trace.0.lock().unwrap();
        let state = guard.as_ref().expect("prior table still installed");
        assert_eq!(state.lock().unwrap().table.entries[0].path, "prior");
    }

    // [test->app~obs_003~1]
    #[test]
    fn accepted_install_replaces_the_table_and_yields_a_consumer() {
        let (result, trace) = run_install(Payload::TraceStatus(TraceStatus {
            ram_budget_bytes_per_ms: 2048,
            ram_usage_bytes_per_ms: 100,
            link_budget_bytes_per_s: 480_000,
            link_rate_bytes_per_s: 25_000,
        }));
        let (status, consumer) = match result {
            Ok(v) => v,
            Err(e) => panic!("accepted install failed: {e}"),
        };
        assert_eq!(status.ram_budget_bytes_per_ms, 2048);
        assert_eq!(status.ram_usage_bytes_per_ms, 100);
        assert!(consumer.is_some());
        let guard = trace.0.lock().unwrap();
        let state = guard.as_ref().expect("new table installed");
        assert_eq!(state.lock().unwrap().table.entries[0].path, "new");
    }
}
