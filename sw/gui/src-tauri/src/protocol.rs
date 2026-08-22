//! Transport-agnostic protocol client: request/reply correlation over the
//! framed envelope stream, with board-initiated streams fanned out to a sink.
//! No serial/tauri coupling here, so the whole client tests headless over
//! in-memory pipes.

use std::collections::HashMap;
use std::io::Write;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{mpsc, Arc, Mutex};
use std::time::Duration;

use prost::Message;

use pcs_proto::shared::envelope::Payload;

/// A board-initiated stream payload (request_id 0).
pub enum StreamEvent {
    Log(pcs_proto::shared::LogText),
    Telemetry(pcs_proto::board::Telemetry),
    Samples(pcs_proto::trace::Samples),
}

pub type StreamSink = Box<dyn Fn(StreamEvent) + Send>;

type Pending = Arc<Mutex<HashMap<u32, mpsc::Sender<Payload>>>>;

pub struct Client {
    writer: Mutex<Box<dyn Write + Send>>,
    pending: Pending,
    next_id: AtomicU32,
}

impl Client {
    pub fn new(writer: Box<dyn Write + Send>) -> Self {
        Self {
            writer: Mutex::new(writer),
            pending: Pending::default(),
            // 0 is the stream sentinel — request ids start at 1.
            next_id: AtomicU32::new(1),
        }
    }

    /// The receive half: feed it into the reader thread with this client's
    /// pending map and the stream sink.
    pub fn pump(&self, sink: StreamSink) -> Pump {
        Pump {
            deframer: pcs_wire::Deframer::new(),
            pending: self.pending.clone(),
            sink,
        }
    }

    // [impl->app~conn_003~1]
    pub fn request(&self, payload: Payload) -> Result<Payload, String> {
        self.request_timeout(payload, Duration::from_millis(500))
    }

    pub fn request_timeout(&self, payload: Payload, timeout: Duration) -> Result<Payload, String> {
        let id = self.next_id.fetch_add(1, Ordering::Relaxed);
        let (tx, rx) = mpsc::channel();
        self.pending
            .lock()
            .map_err(|_| "pending map poisoned".to_string())?
            .insert(id, tx);

        let env = pcs_proto::shared::Envelope {
            request_id: id,
            payload: Some(payload),
        };
        // Write under the lock, wait outside it — concurrent requests must
        // overlap their waits, not serialize on the writer.
        let written = {
            match self.writer.lock() {
                Err(_) => Err("writer poisoned".to_string()),
                Ok(mut writer) => writer
                    .write_all(&pcs_wire::frame(&env.encode_to_vec()))
                    .and_then(|_| writer.flush())
                    .map_err(|e| format!("write: {e}")),
            }
        };
        let result = match written {
            Err(e) => Err(e),
            Ok(()) => rx
                .recv_timeout(timeout)
                .map_err(|_| "timeout waiting for reply".to_string()),
        };
        if let Ok(mut pending) = self.pending.lock() {
            pending.remove(&id);
        }
        result
    }
}

/// The reader-thread half: deframes received bytes, routes replies to their
/// pending requests, and hands stream payloads to the sink. A reply whose
/// request timed out (no pending entry) is dropped.
pub struct Pump {
    deframer: pcs_wire::Deframer,
    pending: Pending,
    sink: StreamSink,
}

impl Pump {
    pub fn push(&mut self, bytes: &[u8]) {
        for payload_bytes in self.deframer.push(bytes) {
            let Ok(env) = pcs_proto::shared::Envelope::decode(payload_bytes.as_slice()) else {
                continue;
            };
            let Some(payload) = env.payload else {
                continue;
            };
            if env.request_id != 0 {
                let waiter = self
                    .pending
                    .lock()
                    .ok()
                    .and_then(|p| p.get(&env.request_id).cloned());
                if let Some(tx) = waiter {
                    let _ = tx.send(payload);
                }
                continue;
            }
            match payload {
                Payload::Log(log) => (self.sink)(StreamEvent::Log(log)),
                Payload::Telemetry(telemetry) => (self.sink)(StreamEvent::Telemetry(telemetry)),
                Payload::Samples(samples) => (self.sink)(StreamEvent::Samples(samples)),
                _ => {}
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// In-memory transmit capture standing in for the serial writer.
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

    fn reply_frame(request_id: u32, payload: Payload) -> Vec<u8> {
        let env = pcs_proto::shared::Envelope {
            request_id,
            payload: Some(payload),
        };
        pcs_wire::frame(&env.encode_to_vec())
    }

    fn response(accepted: bool, cause: &str) -> Payload {
        Payload::Response(pcs_proto::shared::Response {
            accepted,
            cause: cause.into(),
        })
    }

    /// Wait until the capture holds `n` whole frames, returning their decoded
    /// request ids in write order.
    fn wait_for_requests(buf: &SharedBuf, n: usize) -> Vec<u32> {
        for _ in 0..200 {
            let wire = buf.0.lock().unwrap().clone();
            let frames = pcs_wire::parse_frames(&wire);
            if frames.len() >= n {
                return frames
                    .iter()
                    .map(|f| {
                        pcs_proto::shared::Envelope::decode(f.as_slice())
                            .unwrap()
                            .request_id
                    })
                    .collect();
            }
            std::thread::sleep(Duration::from_millis(5));
        }
        panic!("requests never reached the writer");
    }

    // [test->app~conn_003~1]
    #[test]
    fn concurrent_requests_resolve_independently_by_id() {
        let buf = SharedBuf::default();
        let client = Arc::new(Client::new(Box::new(buf.clone())));
        let mut pump = client.pump(Box::new(|_| {}));

        let workers: Vec<_> = (0..2)
            .map(|_| {
                let c = client.clone();
                std::thread::spawn(move || {
                    c.request(Payload::Ping(pcs_proto::shared::PingRequest::default()))
                })
            })
            .collect();

        let ids = wait_for_requests(&buf, 2);
        // Answer in reverse write order with distinguishable verdicts. The
        // workers race to the writer, so wire order need not match spawn
        // order — judge the verdict SET, not per-worker positions.
        pump.push(&reply_frame(ids[1], response(true, "")));
        pump.push(&reply_frame(
            ids[0],
            response(false, "first-written request rejected"),
        ));

        let mut verdicts: Vec<(bool, String)> = workers
            .into_iter()
            .map(|w| match w.join().unwrap() {
                Ok(Payload::Response(r)) => (r.accepted, r.cause),
                other => panic!("wrong resolution: {other:?}"),
            })
            .collect();
        verdicts.sort_by_key(|(accepted, _)| *accepted);
        assert_eq!(
            verdicts,
            vec![
                (false, "first-written request rejected".to_string()),
                (true, String::new()),
            ]
        );
    }

    #[test]
    fn rejected_response_delivers_cause() {
        let buf = SharedBuf::default();
        let client = Arc::new(Client::new(Box::new(buf.clone())));
        let mut pump = client.pump(Box::new(|_| {}));

        let worker = {
            let c = client.clone();
            std::thread::spawn(move || {
                c.request(Payload::Ping(pcs_proto::shared::PingRequest::default()))
            })
        };
        let ids = wait_for_requests(&buf, 1);
        pump.push(&reply_frame(
            ids[0],
            response(false, "watch span not readable"),
        ));

        match worker.join().unwrap() {
            Ok(Payload::Response(r)) => {
                assert!(!r.accepted);
                assert_eq!(r.cause, "watch span not readable");
            }
            other => panic!("wrong resolution: {other:?}"),
        }
    }

    #[test]
    fn streams_dispatch_with_no_outstanding_request() {
        let seen: Arc<Mutex<Vec<&'static str>>> = Arc::default();
        let sink_seen = seen.clone();
        let client = Client::new(Box::new(SharedBuf::default()));
        let mut pump = client.pump(Box::new(move |ev| {
            sink_seen.lock().unwrap().push(match ev {
                StreamEvent::Log(_) => "log",
                StreamEvent::Telemetry(_) => "telemetry",
                StreamEvent::Samples(_) => "samples",
            });
        }));

        pump.push(&reply_frame(
            0,
            Payload::Log(pcs_proto::shared::LogText {
                text: "hi\n".into(),
            }),
        ));
        pump.push(&reply_frame(
            0,
            Payload::Telemetry(pcs_proto::board::Telemetry::default()),
        ));
        pump.push(&reply_frame(
            0,
            Payload::Samples(pcs_proto::trace::Samples {
                tick_ms: 3,
                data: vec![1, 2, 3, 4],
            }),
        ));
        assert_eq!(*seen.lock().unwrap(), vec!["log", "telemetry", "samples"]);
    }

    #[test]
    fn corrupted_frame_between_valid_frames_is_discarded() {
        let seen: Arc<Mutex<Vec<String>>> = Arc::default();
        let sink_seen = seen.clone();
        let client = Client::new(Box::new(SharedBuf::default()));
        let mut pump = client.pump(Box::new(move |ev| {
            if let StreamEvent::Log(l) = ev {
                sink_seen.lock().unwrap().push(l.text);
            }
        }));

        let mut wire = reply_frame(
            0,
            Payload::Log(pcs_proto::shared::LogText { text: "a".into() }),
        );
        let mut bad = reply_frame(
            0,
            Payload::Log(pcs_proto::shared::LogText { text: "x".into() }),
        );
        bad[3] ^= 0x40;
        wire.extend_from_slice(&bad);
        wire.extend_from_slice(&reply_frame(
            0,
            Payload::Log(pcs_proto::shared::LogText { text: "b".into() }),
        ));
        pump.push(&wire);
        assert_eq!(*seen.lock().unwrap(), vec!["a", "b"]);
    }

    #[test]
    fn unanswered_request_times_out() {
        let client = Client::new(Box::new(SharedBuf::default()));
        let result = client.request_timeout(
            Payload::Ping(pcs_proto::shared::PingRequest::default()),
            Duration::from_millis(20),
        );
        assert_eq!(result.unwrap_err(), "timeout waiting for reply");
    }
}
