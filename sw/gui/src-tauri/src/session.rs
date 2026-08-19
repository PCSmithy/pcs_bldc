//! The native core's device session: the serial port and protocol state live
//! here; the webview only presents what these commands return.
//! (app~arch_001: a UI reload must not disturb this state.)

use std::io::{Read, Write};
use std::sync::Mutex;
use std::time::{Duration, Instant};

use prost::Message;
use tauri::State;

pub struct Session {
    port: Box<dyn serialport::SerialPort>,
    deframer: pcs_wire::Deframer,
    next_request_id: u32,
}

#[derive(Default)]
pub struct SessionState(pub Mutex<Option<Session>>);

#[derive(serde::Serialize)]
pub struct PortInfo {
    name: String,
    kind: String,
}

// [impl->app~conn_001~1]
#[tauri::command]
pub fn list_ports() -> Vec<PortInfo> {
    serialport::available_ports()
        .unwrap_or_default()
        .into_iter()
        .map(|p| PortInfo {
            name: p.port_name,
            kind: match p.port_type {
                serialport::SerialPortType::UsbPort(info) => {
                    // Windows' product string ends in a redundant " (COMn)".
                    let product = info
                        .product
                        .map(|s| match s.rfind(" (COM") {
                            Some(cut) => s[..cut].to_string(),
                            None => s,
                        })
                        .unwrap_or_else(|| "USB".to_string());
                    format!("{product} [{:04x}:{:04x}]", info.vid, info.pid)
                }
                _ => "other".into(),
            },
        })
        .collect()
}

/// Open the selected port and greet the board; returns the reported build id.
#[tauri::command]
pub fn connect(state: State<SessionState>, port: String) -> Result<String, String> {
    let mut opened = serialport::new(&port, 115_200)
        .timeout(Duration::from_millis(50))
        .open()
        .map_err(|e| format!("open {port}: {e}"))?;
    // The firmware serves only while the host holds the port open, which it
    // reads from the CDC line state — DTR must be raised explicitly here
    // (pyserial does it implicitly; serialport-rs does not).
    opened
        .write_data_terminal_ready(true)
        .map_err(|e| format!("assert DTR on {port}: {e}"))?;
    let mut session = Session {
        port: opened,
        deframer: pcs_wire::Deframer::new(),
        next_request_id: 1,
    };
    let build_id = session.identity()?;
    *state.0.lock().unwrap() = Some(session);
    Ok(build_id)
}

#[tauri::command]
pub fn disconnect(state: State<SessionState>) {
    *state.0.lock().unwrap() = None;
}

impl Session {
    fn send(&mut self, payload: pcs_proto::shared::envelope::Payload) -> Result<u32, String> {
        let id = self.next_request_id;
        self.next_request_id += 1;
        let env = pcs_proto::shared::Envelope {
            request_id: id,
            payload: Some(payload),
        };
        self.port
            .write_all(&pcs_wire::frame(&env.encode_to_vec()))
            .map_err(|e| format!("write: {e}"))?;
        Ok(id)
    }

    /// Pump the port until the reply carrying `id` arrives; stream envelopes
    /// (log, telemetry, samples) pass through undelivered for now.
    fn wait_reply(
        &mut self,
        id: u32,
        timeout: Duration,
    ) -> Result<pcs_proto::shared::envelope::Payload, String> {
        let deadline = Instant::now() + timeout;
        let mut buf = [0u8; 1024];
        while Instant::now() < deadline {
            let n = match self.port.read(&mut buf) {
                Ok(n) => n,
                Err(e) if e.kind() == std::io::ErrorKind::TimedOut => 0,
                Err(e) => return Err(format!("read: {e}")),
            };
            for payload_bytes in self.deframer.push(&buf[..n]) {
                if let Ok(env) = pcs_proto::shared::Envelope::decode(payload_bytes.as_slice()) {
                    if env.request_id == id {
                        return env.payload.ok_or_else(|| "empty reply".to_string());
                    }
                }
            }
        }
        Err("timeout waiting for reply".to_string())
    }

    fn identity(&mut self) -> Result<String, String> {
        let id = self.send(pcs_proto::shared::envelope::Payload::IdentityRequest(
            pcs_proto::shared::IdentityRequest::default(),
        ))?;
        match self.wait_reply(id, Duration::from_millis(500))? {
            pcs_proto::shared::envelope::Payload::Identity(identity) => Ok(identity.build_id),
            other => Err(format!("unexpected reply: {other:?}")),
        }
    }
}
