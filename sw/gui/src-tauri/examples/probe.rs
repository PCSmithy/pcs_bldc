//! Headless smoke probe for the session stack: open a port, assert DTR, run
//! the identity round trip, print the build id. Usage:
//!
//! ```text
//! cargo run --example probe -- COM8
//! ```

use std::io::{Read, Write};
use std::time::{Duration, Instant};

use prost::Message;

fn main() {
    let port_name = std::env::args().nth(1).expect("usage: probe <PORT>");
    let mut port = serialport::new(&port_name, 115_200)
        .timeout(Duration::from_millis(50))
        .open()
        .unwrap_or_else(|e| panic!("open {port_name}: {e}"));
    port.write_data_terminal_ready(true).expect("assert DTR");

    let env = pcs_proto::shared::Envelope {
        request_id: 1,
        payload: Some(pcs_proto::shared::envelope::Payload::IdentityRequest(
            pcs_proto::shared::IdentityRequest::default(),
        )),
    };
    port.write_all(&pcs_wire::frame(&env.encode_to_vec()))
        .expect("write identity request");

    let mut deframer = pcs_wire::Deframer::new();
    let deadline = Instant::now() + Duration::from_millis(1000);
    let mut buf = [0u8; 1024];
    while Instant::now() < deadline {
        let n = match port.read(&mut buf) {
            Ok(n) => n,
            Err(e) if e.kind() == std::io::ErrorKind::TimedOut => 0,
            Err(e) => panic!("read: {e}"),
        };
        for payload in deframer.push(&buf[..n]) {
            let Ok(env) = pcs_proto::shared::Envelope::decode(payload.as_slice()) else {
                continue;
            };
            match env.payload {
                Some(pcs_proto::shared::envelope::Payload::Identity(identity))
                    if env.request_id == 1 =>
                {
                    println!("identity: {}", identity.build_id);
                    return;
                }
                Some(other) => println!("(stream) {other:?}"),
                None => {}
            }
        }
    }
    panic!("timeout: no identity reply within 1 s");
}
