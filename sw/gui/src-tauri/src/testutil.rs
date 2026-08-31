//! Shared unit-test plumbing: the in-memory transmit capture and the
//! wait-for-request-frames poll that protocol.rs and trace.rs tests drive.

use std::io::Write;
use std::sync::{Arc, Mutex};
use std::time::Duration;

/// In-memory transmit capture standing in for the serial writer.
#[derive(Clone, Default)]
pub struct SharedBuf(pub Arc<Mutex<Vec<u8>>>);

impl Write for SharedBuf {
    fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
        self.0.lock().unwrap().extend_from_slice(buf);
        Ok(buf.len())
    }
    fn flush(&mut self) -> std::io::Result<()> {
        Ok(())
    }
}

/// Wait until the capture holds `n` whole frames, returning their decoded
/// request ids in write order.
pub fn wait_for_requests(buf: &SharedBuf, n: usize) -> Vec<u32> {
    use prost::Message;
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
