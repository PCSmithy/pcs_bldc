//! The trace client: watch installation behind the identity gate, `Samples`
//! demultiplexing per the phase-locked due rule, and batched "samples"
//! events toward the webview (raw stream rate is 1 kHz; the UI gets ~20 Hz).

use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use tauri::{AppHandle, Emitter, State};

use crate::firmware::{identity_matches, FirmwareState};
use crate::protocol::Client;
use crate::session::{SamplesConsumer, SessionState};

/// Batched samples flush thresholds: whichever trips first.
const BATCH_EMIT_INTERVAL: Duration = Duration::from_millis(50);
const BATCH_EMIT_MAX_MSGS: usize = 256;

#[derive(serde::Deserialize)]
pub struct WatchSpec {
    pub path: String,
    pub period_ms: u32,
}

#[derive(Clone)]
struct WatchEntry {
    path: String,
    size: u32,
    period_ms: u32,
    leaf: dwarf_map::Leaf,
}

struct WatchTable {
    entries: Vec<WatchEntry>,
}

#[derive(Clone, serde::Serialize)]
pub struct TraceStatusInfo {
    pub ram_budget_bytes: u32,
    pub ram_worst_tick_bytes: u32,
    pub link_budget_bytes_per_s: u32,
    pub link_rate_bytes_per_s: u32,
}

impl From<pcs_proto::trace::TraceStatus> for TraceStatusInfo {
    fn from(ts: pcs_proto::trace::TraceStatus) -> Self {
        Self {
            ram_budget_bytes: ts.ram_budget_bytes,
            ram_worst_tick_bytes: ts.ram_worst_tick_bytes,
            link_budget_bytes_per_s: ts.link_budget_bytes_per_s,
            link_rate_bytes_per_s: ts.link_rate_bytes_per_s,
        }
    }
}

#[derive(Clone, serde::Serialize)]
struct SignalSeries {
    path: String,
    points: Vec<(u32, f64)>,
}

#[derive(Clone, serde::Serialize)]
pub struct SamplesBatch {
    signals: Vec<SignalSeries>,
    dropped_ticks: u32,
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

/// Demultiplex one `Samples` message: bytes belong, in watch-list order, to
/// exactly the entries whose period divides the tick. A length mismatch
/// between the due set and the data means a corrupt or foreign message —
/// dropped whole.
// [impl->app~obs_004~1]
fn demux(table: &WatchTable, samples: &pcs_proto::trace::Samples) -> Vec<(usize, u32, f64)> {
    let due: Vec<usize> = table
        .entries
        .iter()
        .enumerate()
        .filter(|(_, e)| samples.tick_ms % e.period_ms == 0)
        .map(|(i, _)| i)
        .collect();
    let expected: usize = due.iter().map(|&i| table.entries[i].size as usize).sum();
    if expected != samples.data.len() {
        return Vec::new();
    }
    let mut out = Vec::with_capacity(due.len());
    let mut offset = 0usize;
    for i in due {
        let entry = &table.entries[i];
        let bytes = &samples.data[offset..offset + entry.size as usize];
        offset += entry.size as usize;
        out.push((i, samples.tick_ms, decode(entry.leaf, bytes)));
    }
    out
}

/// The consumer-side accumulator: demuxed points per entry, flushed as one
/// "samples" event when a threshold trips.
struct BatchState {
    table: WatchTable,
    buffers: Vec<Vec<(u32, f64)>>,
    min_period_ms: u32,
    prev_tick: Option<u32>,
    dropped_ticks: u32,
    msgs_since_emit: usize,
    last_emit: Instant,
}

impl BatchState {
    fn new(table: WatchTable) -> Self {
        let buffers = table.entries.iter().map(|_| Vec::new()).collect();
        // Periods are nested (1 | 10 | 100), so every due tick is a multiple
        // of the fastest period — the stride the gap counter measures in.
        let min_period_ms = table.entries.iter().map(|e| e.period_ms).min().unwrap_or(1);
        Self {
            table,
            buffers,
            min_period_ms,
            prev_tick: None,
            dropped_ticks: 0,
            msgs_since_emit: 0,
            last_emit: Instant::now(),
        }
    }

    fn ingest(&mut self, samples: &pcs_proto::trace::Samples) -> Option<SamplesBatch> {
        let points = demux(&self.table, samples);
        if points.is_empty() {
            return None;
        }
        if let Some(prev) = self.prev_tick {
            let stride = self.min_period_ms;
            if samples.tick_ms > prev + stride {
                self.dropped_ticks += (samples.tick_ms - prev) / stride - 1;
            }
        }
        self.prev_tick = Some(samples.tick_ms);
        for (entry, tick, value) in points {
            self.buffers[entry].push((tick, value));
        }
        self.msgs_since_emit += 1;
        if self.msgs_since_emit >= BATCH_EMIT_MAX_MSGS
            || self.last_emit.elapsed() >= BATCH_EMIT_INTERVAL
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
            dropped_ticks: self.dropped_ticks,
        };
        self.dropped_ticks = 0;
        self.msgs_since_emit = 0;
        self.last_emit = Instant::now();
        batch
    }
}

#[derive(Default)]
pub struct TraceState(Mutex<Option<Arc<Mutex<BatchState>>>>);

/// Flush any prior accumulator through `emit` (residual points would
/// otherwise vanish on a list change) and uninstall it.
fn flush_prior(trace: &TraceState, emit: &dyn Fn(SamplesBatch)) {
    let prior = trace.0.lock().ok().and_then(|mut slot| slot.take());
    if let Some(prior) = prior {
        if let Ok(mut state) = prior.lock() {
            let batch = state.flush();
            if !batch.signals.is_empty() {
                emit(batch);
            }
        }
    }
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
    emit: Box<dyn Fn(SamplesBatch) + Send>,
) -> Result<(TraceStatusInfo, Option<SamplesConsumer>), String> {
    use pcs_proto::shared::envelope::Payload;
    let reply = client.request(Payload::WatchRequest(pcs_proto::trace::WatchRequest {
        watches: wire_watches,
    }))?;
    let status = match reply {
        Payload::TraceStatus(ts) => TraceStatusInfo::from(ts),
        Payload::Response(r) => {
            return Err(if r.cause.is_empty() {
                "watch list rejected".to_string()
            } else {
                r.cause
            })
        }
        other => return Err(format!("unexpected reply: {other:?}")),
    };

    flush_prior(trace, emit.as_ref());
    let consumer = if entries.is_empty() {
        // An accepted empty list stops the stream: nothing to install.
        None
    } else {
        let shared = Arc::new(Mutex::new(BatchState::new(WatchTable { entries })));
        let consumer_shared = shared.clone();
        let consumer: SamplesConsumer = Box::new(move |samples| {
            let batch = consumer_shared
                .lock()
                .ok()
                .and_then(|mut state| state.ingest(&samples));
            if let Some(batch) = batch {
                emit(batch);
            }
        });
        if let Ok(mut slot) = trace.0.lock() {
            *slot = Some(shared);
        }
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
) -> Result<TraceStatusInfo, String> {
    for w in &watches {
        if !matches!(w.period_ms, 1 | 10 | 100) {
            return Err(format!(
                "{}: period {} ms is not 1/10/100",
                w.path, w.period_ms
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
                period_ms: w.period_ms,
            });
            entries.push(WatchEntry {
                path: w.path.clone(),
                size,
                period_ms: w.period_ms,
                leaf,
            });
        }
        (loaded.build_id.clone(), wire, entries)
    };

    // Session handles (lock scope: session only; the request runs unlocked).
    let (client, device_build_id) = {
        let guard = session.0.lock().map_err(|_| "session state poisoned")?;
        let session = guard.as_ref().ok_or("not connected")?;
        (
            session.client.clone(),
            session.device_build_id().to_string(),
        )
    };

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

    if let Ok(guard) = session.0.lock() {
        if let Some(session) = guard.as_ref() {
            session.set_samples_consumer(consumer);
        }
    }
    let _ = app.emit("trace-status", status.clone());
    Ok(status)
}

#[tauri::command]
pub fn clear_watches(
    app: AppHandle,
    session: State<SessionState>,
    trace: State<TraceState>,
) -> Result<TraceStatusInfo, String> {
    let client = {
        let guard = session.0.lock().map_err(|_| "session state poisoned")?;
        let session = guard.as_ref().ok_or("not connected")?;
        session.set_samples_consumer(None);
        session.client.clone()
    };
    let flush_app = app.clone();
    flush_prior(&trace, &move |batch| {
        let _ = flush_app.emit("samples", batch);
    });

    use pcs_proto::shared::envelope::Payload;
    let reply = client.request(Payload::WatchRequest(pcs_proto::trace::WatchRequest {
        watches: Vec::new(),
    }))?;
    let status = match reply {
        Payload::TraceStatus(ts) => TraceStatusInfo::from(ts),
        Payload::Response(r) => return Err(r.cause),
        other => return Err(format!("unexpected reply: {other:?}")),
    };
    let _ = app.emit("trace-status", status.clone());
    Ok(status)
}

#[tauri::command]
pub fn trace_status(session: State<SessionState>) -> Result<TraceStatusInfo, String> {
    let client = {
        let guard = session.0.lock().map_err(|_| "session state poisoned")?;
        guard.as_ref().ok_or("not connected")?.client.clone()
    };
    use pcs_proto::shared::envelope::Payload;
    match client.request(Payload::TraceStatusRequest(
        pcs_proto::trace::TraceStatusRequest::default(),
    ))? {
        Payload::TraceStatus(ts) => Ok(TraceStatusInfo::from(ts)),
        other => Err(format!("unexpected reply: {other:?}")),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use dwarf_map::{Leaf, Scalar};

    fn entry(path: &str, size: u32, period_ms: u32, leaf: Leaf) -> WatchEntry {
        WatchEntry {
            path: path.into(),
            size,
            period_ms,
            leaf,
        }
    }

    fn samples(tick_ms: u32, data: &[u8]) -> pcs_proto::trace::Samples {
        pcs_proto::trace::Samples {
            tick_ms,
            data: data.to_vec(),
        }
    }

    fn mixed_table() -> WatchTable {
        WatchTable {
            entries: vec![
                entry("a", 4, 1, Leaf::Scalar(Scalar::U32)),
                entry("b", 2, 10, Leaf::Scalar(Scalar::U16)),
                entry("c", 1, 100, Leaf::Scalar(Scalar::U8)),
            ],
        }
    }

    // [test->app~obs_004~1]
    #[test]
    fn membership_follows_the_due_rule_in_list_order() {
        let table = mixed_table();
        // Tick 0: everything due, list order a|b|c.
        let got = demux(
            &table,
            &samples(0, &[0x44, 0x33, 0x22, 0x11, 0xEF, 0xBE, 0x7F]),
        );
        assert_eq!(
            got,
            vec![
                (0, 0, f64::from(0x1122_3344u32)),
                (1, 0, f64::from(0xBEEFu16)),
                (2, 0, f64::from(0x7Fu8)),
            ]
        );
        // Tick 1: only the 1 ms entry.
        assert_eq!(demux(&table, &samples(1, &[1, 0, 0, 0])), vec![(0, 1, 1.0)]);
        // Tick 10: 1 ms + 10 ms.
        assert_eq!(
            demux(&table, &samples(10, &[2, 0, 0, 0, 5, 0])),
            vec![(0, 10, 2.0), (1, 10, 5.0)]
        );
        // Tick 100: all three again.
        assert_eq!(
            demux(&table, &samples(100, &[3, 0, 0, 0, 6, 0, 9])),
            vec![(0, 100, 3.0), (1, 100, 6.0), (2, 100, 9.0)]
        );
    }

    // [test->app~obs_004~1]
    #[test]
    fn gap_in_ticks_yields_points_only_at_received_ticks() {
        let mut state = BatchState::new(WatchTable {
            entries: vec![entry("a", 1, 1, Leaf::Scalar(Scalar::U8))],
        });
        for tick in [0u32, 1, 5, 6] {
            // Re-arm the interval clock so a stalled test host can't trigger
            // an early flush and steal points from the final one.
            state.last_emit = Instant::now();
            let _ = state.ingest(&samples(tick, &[tick as u8]));
        }
        let batch = state.flush();
        assert_eq!(batch.signals.len(), 1);
        assert_eq!(
            batch.signals[0].points,
            vec![(0, 0.0), (1, 1.0), (5, 5.0), (6, 6.0)]
        );
        // Ticks 2, 3, 4 never arrived: counted as dropped, not synthesized.
        assert_eq!(batch.dropped_ticks, 3);
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
        ];
        for (leaf, bytes, expected) in cases {
            let table = WatchTable {
                entries: vec![entry("x", bytes.len() as u32, 1, leaf)],
            };
            let got = demux(&table, &samples(0, &bytes));
            assert_eq!(got.len(), 1, "{leaf:?}");
            assert_eq!(got[0].2, expected, "{leaf:?}");
        }
    }

    // [test->app~obs_004~1]
    #[test]
    fn length_mismatch_drops_the_whole_message() {
        let table = mixed_table();
        // Tick 0 expects 7 bytes; 6 (or 8) means foreign membership — drop.
        assert!(demux(&table, &samples(0, &[0; 6])).is_empty());
        assert!(demux(&table, &samples(0, &[0; 8])).is_empty());
        // And a valid message right after still demuxes.
        assert_eq!(demux(&table, &samples(1, &[9, 0, 0, 0])).len(), 1);
    }

    #[test]
    fn batch_emits_at_the_message_threshold() {
        let mut state = BatchState::new(WatchTable {
            entries: vec![entry("a", 1, 1, Leaf::Scalar(Scalar::U8))],
        });
        // Re-arm the interval clock each message so only the count threshold
        // can trip, however slowly the test host runs.
        let mut emitted = None;
        for tick in 0..BATCH_EMIT_MAX_MSGS as u32 {
            state.last_emit = Instant::now();
            if let Some(batch) = state.ingest(&samples(tick, &[0])) {
                emitted = Some((tick, batch));
            }
        }
        let (at_tick, batch) = emitted.expect("threshold emit");
        assert_eq!(at_tick, BATCH_EMIT_MAX_MSGS as u32 - 1);
        assert_eq!(batch.signals[0].points.len(), BATCH_EMIT_MAX_MSGS);
        assert_eq!(state.msgs_since_emit, 0);
    }

    /// Drive `perform_install` against a mock wire that answers the watch
    /// request with `reply`, and return the outcome plus the trace state.
    fn run_install(
        reply: pcs_proto::shared::envelope::Payload,
    ) -> (
        Result<(TraceStatusInfo, Option<SamplesConsumer>), String>,
        TraceState,
    ) {
        use pcs_proto::shared::envelope::Payload;
        use prost::Message;
        use std::io::Write;

        #[derive(Clone, Default)]
        struct SharedBuf(Arc<Mutex<Vec<u8>>>);
        impl Write for SharedBuf {
            fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
                self.0.lock().unwrap().extend_from_slice(buf);
                Ok(buf.len())
            }
            fn flush(&mut self) -> std::io::Result<()> {
                Ok(())
            }
        }

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
                        period_ms: 1,
                    }],
                    vec![entry("new", 4, 1, Leaf::Scalar(Scalar::U32))],
                    Box::new(|_| {}),
                )
            });
            // Answer the request once it hits the mock wire.
            let request_id = loop {
                let wire = buf.0.lock().unwrap().clone();
                if let Some(frame) = pcs_wire::parse_frames(&wire).first() {
                    break pcs_proto::shared::Envelope::decode(frame.as_slice())
                        .unwrap()
                        .request_id;
                }
                std::thread::sleep(Duration::from_millis(2));
            };
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
        use pcs_proto::shared::envelope::Payload;

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
        use pcs_proto::shared::envelope::Payload;

        let (result, trace) = run_install(Payload::TraceStatus(pcs_proto::trace::TraceStatus {
            ram_budget_bytes: 2048,
            ram_worst_tick_bytes: 8,
            link_budget_bytes_per_s: 1_100_000,
            link_rate_bytes_per_s: 25_000,
        }));
        let (status, consumer) = match result {
            Ok(v) => v,
            Err(e) => panic!("accepted install failed: {e}"),
        };
        assert_eq!(status.ram_budget_bytes, 2048);
        assert!(consumer.is_some());
        let guard = trace.0.lock().unwrap();
        let state = guard.as_ref().expect("new table installed");
        assert_eq!(state.lock().unwrap().table.entries[0].path, "new");
    }
}
