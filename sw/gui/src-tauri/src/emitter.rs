//! Off-thread UI emission. A webview emit needs the main thread, so emitting
//! inline from the reader thread stalls it on the first telemetry frame and
//! the reply bytes queued behind that frame are never parsed — the request
//! then times out although the device answered in milliseconds. Every event
//! the reader produces crosses this bounded queue to one emitter thread
//! instead, and a full queue sheds by a policy chosen per event kind.

use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::mpsc::{sync_channel, SyncSender, TrySendError};
use std::sync::Arc;

use crate::session::{ConnectionEvent, LogEvent, TelemetryEvent};
use crate::trace::SamplesBatch;

/// Queue depth: a reader stalled on a slow renderer must fill this, not the
/// OS receive buffer and then the device FIFO — past that the BOARD starts
/// dropping records, the failure this decoupling exists to prevent.
const EMIT_QUEUE_DEPTH: usize = 32;

pub enum UiEvent {
    Samples(SamplesBatch),
    Telemetry(TelemetryEvent),
    Log(LogEvent),
    Connection(ConnectionEvent),
}

// [impl->app~conn_003~1]
pub struct UiEmitter {
    tx: SyncSender<UiEvent>,
    /// Sample points shed on a full queue, charged to the next batch through.
    shed_points: AtomicU64,
    /// Log lines shed on a full queue, surfaced as one line when room returns.
    dropped_logs: AtomicU64,
    stop: Arc<AtomicBool>,
}

impl UiEmitter {
    pub fn new(sink: Box<dyn Fn(UiEvent) + Send + 'static>) -> Self {
        let (tx, rx) = sync_channel::<UiEvent>(EMIT_QUEUE_DEPTH);
        let stop = Arc::new(AtomicBool::new(false));
        let thread_stop = stop.clone();
        if let Err(e) = std::thread::Builder::new()
            .name("ui-emitter".into())
            .spawn(move || {
                // Ends when every sender drops; the flag discards whatever a
                // torn-down session left queued.
                for event in rx {
                    if thread_stop.load(Ordering::Relaxed) {
                        break;
                    }
                    sink(event);
                }
            })
        {
            // No emitter thread: every send sheds, and the tallies keep
            // accumulating honestly rather than blocking the reader.
            eprintln!("spawn ui emitter: {e}");
        }
        Self {
            tx,
            shed_points: AtomicU64::new(0),
            dropped_logs: AtomicU64::new(0),
            stop,
        }
    }

    /// Queue one event under its kind's full-queue policy. Only a connection
    /// edge ever waits.
    pub fn send(&self, event: UiEvent) {
        match event {
            UiEvent::Samples(batch) => self.send_samples(batch),
            // Superseded by the next frame 100 ms later: dropping one costs
            // nothing a retry would buy back.
            UiEvent::Telemetry(t) => {
                let _ = self.tx.try_send(UiEvent::Telemetry(t));
            }
            UiEvent::Log(log) => self.send_log(log),
            // Rare, and an edge the UI misses leaves it wrong until the next
            // one — worth waiting for room. Only ever sent by a thread that
            // is already done reading.
            UiEvent::Connection(c) => {
                let _ = self.tx.send(UiEvent::Connection(c));
            }
        }
    }

    /// A shed batch charges its points to the next one that fits, so host-side
    /// shedding reaches the user as gaps rather than as silence.
    fn send_samples(&self, mut batch: SamplesBatch) {
        let points = batch.point_count();
        batch.host_dropped_points = self.shed_points.swap(0, Ordering::Relaxed);
        if let Err(e) = self.tx.try_send(UiEvent::Samples(batch)) {
            let shed = match e {
                TrySendError::Full(UiEvent::Samples(b))
                | TrySendError::Disconnected(UiEvent::Samples(b)) => b.host_dropped_points,
                _ => 0,
            };
            self.shed_points.fetch_add(points + shed, Ordering::Relaxed);
        }
    }

    /// Dropped lines are counted, then surfaced as one line ahead of the next
    /// line that fits: the log reads as lossy rather than as complete.
    fn send_log(&self, log: LogEvent) {
        let dropped = self.dropped_logs.swap(0, Ordering::Relaxed);
        if dropped > 0 {
            let notice = LogEvent {
                text: format!("{dropped} log lines dropped"),
            };
            if self.tx.try_send(UiEvent::Log(notice)).is_err() {
                self.dropped_logs.fetch_add(dropped, Ordering::Relaxed);
            }
        }
        if self.tx.try_send(UiEvent::Log(log)).is_err() {
            self.dropped_logs.fetch_add(1, Ordering::Relaxed);
        }
    }

    /// Discard whatever is still queued: it belongs to a dead session and must
    /// not land in the next one. The thread is never joined, so teardown does
    /// not wait on an emit stalled in the webview.
    pub fn shut_down(&self) {
        self.stop.store(true, Ordering::Relaxed);
    }
}

impl Drop for UiEmitter {
    /// Last handle gone (an installed consumer can outlive the session slot):
    /// stop, and let `tx` dropping with self end the thread.
    fn drop(&mut self) {
        self.shut_down();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{Mutex, MutexGuard};
    use std::time::{Duration, Instant};

    /// A sink parked inside `gate` while the test holds it, so the queue below
    /// it fills deterministically, recording what gets through.
    struct Harness {
        emitter: UiEmitter,
        gate: Arc<Mutex<()>>,
        seen: Arc<Mutex<Vec<UiEvent>>>,
        entered: Arc<AtomicU64>,
    }

    impl Harness {
        fn new() -> Self {
            let gate = Arc::new(Mutex::new(()));
            let seen: Arc<Mutex<Vec<UiEvent>>> = Arc::default();
            let entered = Arc::new(AtomicU64::new(0));
            let sink_gate = gate.clone();
            let sink_seen = seen.clone();
            let sink_entered = entered.clone();
            let emitter = UiEmitter::new(Box::new(move |event| {
                sink_entered.fetch_add(1, Ordering::Relaxed);
                {
                    let _open = sink_gate
                        .lock()
                        .unwrap_or_else(std::sync::PoisonError::into_inner);
                }
                sink_seen
                    .lock()
                    .unwrap_or_else(std::sync::PoisonError::into_inner)
                    .push(event);
            }));
            Self {
                emitter,
                gate,
                seen,
                entered,
            }
        }

        /// Hold the gate with the sink parked in it: one event is taken and
        /// held there, so nothing below it drains until the guard is dropped.
        /// Without the park, an emitter thread that starts late frees a slot
        /// and the queue under test is not actually full.
        fn park(&self) -> MutexGuard<'_, ()> {
            let held = self.gate.lock().unwrap();
            self.emitter
                .send(UiEvent::Connection(ConnectionEvent::down("park")));
            while self.entered.load(Ordering::Relaxed) == 0 {
                std::thread::yield_now();
            }
            held
        }

        /// Drain: wait until the sink has been idle with nothing left to see.
        fn settle(&self) {
            let deadline = Instant::now() + Duration::from_secs(5);
            let mut stable = 0;
            let mut last = 0;
            while (stable < 10) && (Instant::now() < deadline) {
                std::thread::sleep(Duration::from_millis(2));
                let now = self.seen.lock().unwrap().len();
                stable = if now == last { stable + 1 } else { 0 };
                last = now;
            }
        }

        fn logs(&self) -> Vec<String> {
            self.seen
                .lock()
                .unwrap()
                .iter()
                .filter_map(|e| match e {
                    UiEvent::Log(l) => Some(l.text.clone()),
                    _ => None,
                })
                .collect()
        }
    }

    fn samples_batch(points: usize) -> SamplesBatch {
        SamplesBatch {
            signals: vec![crate::trace::SignalSeries {
                path: "a".into(),
                points: vec![(0.0, 0.0); points],
            }],
            dropped_records: 0,
            host_dropped_points: 0,
        }
    }

    fn telemetry(timestamp_ms: u32) -> UiEvent {
        UiEvent::Telemetry(TelemetryEvent::from(pcs_proto::board::Telemetry {
            timestamp_ms,
            ..Default::default()
        }))
    }

    /// A full queue sheds its batch instead of blocking the reader, and the
    /// shed points come back in the next batch that gets through: every point
    /// is either delivered or reported.
    // [test->app~conn_003~1]
    #[test]
    fn a_full_queue_sheds_samples_and_charges_them_to_the_next_batch() {
        const POINTS: usize = 10;
        const SENT: usize = EMIT_QUEUE_DEPTH + 12;
        let h = Harness::new();
        let held = h.park();

        for _ in 0..SENT {
            h.emitter.send(UiEvent::Samples(samples_batch(POINTS)));
        }
        assert_eq!(
            h.emitter.shed_points.load(Ordering::Relaxed),
            ((SENT - EMIT_QUEUE_DEPTH) * POINTS) as u64,
            "everything past the depth sheds, and nothing blocks"
        );

        // Release the sink and keep offering batches until one carries the
        // shed tally through.
        drop(held);
        let mut extra = 0usize;
        while h.emitter.shed_points.load(Ordering::Relaxed) != 0 {
            h.emitter.send(UiEvent::Samples(samples_batch(POINTS)));
            extra += 1;
            assert!(extra < 500, "the shed count never got through");
            std::thread::sleep(Duration::from_millis(1));
        }
        h.settle();

        let delivered: u64 = h
            .seen
            .lock()
            .unwrap()
            .iter()
            .filter_map(|e| match e {
                UiEvent::Samples(b) => Some(b.point_count() + b.host_dropped_points),
                _ => None,
            })
            .sum();
        assert_eq!(
            delivered + h.emitter.shed_points.load(Ordering::Relaxed),
            ((SENT + extra) * POINTS) as u64,
            "every point is delivered or reported shed"
        );
    }

    /// Telemetry is dropped outright on a full queue — the next frame
    /// supersedes it — and never blocks the sender.
    // [test->app~conn_003~1]
    #[test]
    fn a_full_queue_drops_telemetry_without_blocking() {
        const SENT: u32 = (EMIT_QUEUE_DEPTH as u32) + 50;
        let h = Harness::new();
        let held = h.park();
        for ts in 1..=SENT {
            h.emitter.send(telemetry(ts));
        }
        drop(held);
        h.settle();

        let seen: Vec<u32> = h
            .seen
            .lock()
            .unwrap()
            .iter()
            .filter_map(|e| match e {
                UiEvent::Telemetry(t) => Some(t.timestamp_ms),
                _ => None,
            })
            .collect();
        assert_eq!(
            seen.len(),
            EMIT_QUEUE_DEPTH,
            "a full queue drops telemetry rather than blocking"
        );
        // What survived is in order and never duplicated: dropping loses
        // frames, it does not reorder or replay them.
        assert!(seen.windows(2).all(|w| w[0] < w[1]));
    }

    /// Dropped log lines are counted and surfaced once, ahead of the next line
    /// that fits.
    // [test->app~conn_003~1]
    #[test]
    fn a_full_queue_drops_log_lines_and_surfaces_the_count() {
        const SENT: usize = EMIT_QUEUE_DEPTH + 40;
        let h = Harness::new();
        let held = h.park();
        for i in 0..SENT {
            h.emitter.send(UiEvent::Log(LogEvent {
                text: format!("line {i}"),
            }));
        }
        let dropped = h.emitter.dropped_logs.load(Ordering::Relaxed);
        assert_eq!(
            dropped,
            (SENT - EMIT_QUEUE_DEPTH) as u64,
            "a full queue drops log lines rather than blocking"
        );

        drop(held);
        h.settle();
        h.emitter.send(UiEvent::Log(LogEvent {
            text: "after".into(),
        }));
        h.settle();

        let logs = h.logs();
        let notice = format!("{dropped} log lines dropped");
        let at = logs.iter().position(|l| *l == notice).expect("drop notice");
        assert_eq!(logs[at + 1], "after", "the notice precedes the next line");
        assert_eq!(
            logs.iter().filter(|l| **l == notice).count(),
            1,
            "the count is surfaced once"
        );
        assert_eq!(h.emitter.dropped_logs.load(Ordering::Relaxed), 0);
    }

    /// A connection edge waits for room rather than being lost: the UI would
    /// otherwise stay connected over a dead link.
    // [test->app~conn_003~1]
    #[test]
    fn a_connection_event_survives_a_full_queue() {
        let h = Harness::new();
        let held = h.park();
        for _ in 0..(EMIT_QUEUE_DEPTH + 20) {
            h.emitter.send(UiEvent::Samples(samples_batch(1)));
        }
        let emitter = &h.emitter;
        std::thread::scope(|scope| {
            let sender = scope.spawn(move || {
                emitter.send(UiEvent::Connection(ConnectionEvent::down("lost")));
            });
            std::thread::sleep(Duration::from_millis(20));
            assert!(!sender.is_finished(), "a full queue must block this send");
            drop(held);
        });
        h.settle();

        assert!(
            h.seen
                .lock()
                .unwrap()
                .iter()
                .any(|e| matches!(e, UiEvent::Connection(c) if c.state == "lost")),
            "the connection edge reaches the webview"
        );
    }
}
