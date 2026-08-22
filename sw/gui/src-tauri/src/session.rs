//! The native core's device session: the serial port, reader thread, and
//! protocol state live here; the webview only presents the command results
//! and the "connection" / "log" / "telemetry" events.
//! (app~arch_001: a UI reload must not disturb this state.)

use std::io::Read;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;
use std::time::Duration;

use tauri::{AppHandle, Emitter, State};

use crate::protocol::{Client, StreamEvent};

/// Trace-client seam: once installed, receives every raw `Samples` stream
/// message. Demultiplexing is its owner's concern, not the session's.
pub type SamplesConsumer = Box<dyn Fn(pcs_proto::trace::Samples) + Send>;

pub struct Session {
    pub client: Arc<Client>,
    port_name: String,
    device_build_id: String,
    samples_consumer: Arc<Mutex<Option<SamplesConsumer>>>,
    shutdown: Arc<AtomicBool>,
    reader: Option<JoinHandle<()>>,
}

impl Session {
    /// Install the trace client's raw-Samples consumer (replacing any prior).
    pub fn set_samples_consumer(&self, consumer: Option<SamplesConsumer>) {
        if let Ok(mut slot) = self.samples_consumer.lock() {
            *slot = consumer;
        }
    }

    /// The build identity the device reported at connect.
    pub fn device_build_id(&self) -> &str {
        &self.device_build_id
    }
}

#[derive(Default)]
pub struct SessionState(pub Mutex<Option<Session>>);

#[derive(serde::Serialize)]
pub struct PortInfo {
    name: String,
    kind: String,
}

#[derive(Clone, serde::Serialize)]
struct ConnectionEvent {
    state: &'static str,
    port: Option<String>,
    build_id: Option<String>,
}

#[derive(Clone, serde::Serialize)]
struct LogEvent {
    text: String,
}

#[derive(Clone, serde::Serialize)]
struct TelemetryEvent {
    timestamp_ms: u32,
    mode: String,
    state: String,
    bus_voltage_v: f32,
    bus_current_a: f32,
    velocity_measured_radps: f32,
    velocity_setpoint_radps: f32,
}

impl From<pcs_proto::board::Telemetry> for TelemetryEvent {
    fn from(t: pcs_proto::board::Telemetry) -> Self {
        Self {
            timestamp_ms: t.timestamp_ms,
            mode: pcs_proto::board::Mode::try_from(t.mode)
                .map(|m| m.as_str_name().to_string())
                .unwrap_or_else(|_| format!("MODE_{}", t.mode)),
            state: pcs_proto::board::DriveState::try_from(t.state)
                .map(|s| s.as_str_name().to_string())
                .unwrap_or_else(|_| format!("STATE_{}", t.state)),
            bus_voltage_v: t.bus_voltage_v,
            bus_current_a: t.bus_current_a,
            velocity_measured_radps: t.velocity_measured_radps,
            velocity_setpoint_radps: t.velocity_setpoint_radps,
        }
    }
}

#[derive(serde::Serialize)]
pub struct SessionStatus {
    connected: bool,
    port: Option<String>,
    device_build_id: Option<String>,
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

/// Open the selected port, start the reader thread, and greet the board;
/// returns the reported build id.
// [impl->app~conn_001~1]
#[tauri::command]
pub fn connect(app: AppHandle, state: State<SessionState>, port: String) -> Result<String, String> {
    // A fresh connect replaces any existing session whole.
    teardown(&state);

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
    let writer = opened
        .try_clone()
        .map_err(|e| format!("clone {port} for writing: {e}"))?;

    let client = Arc::new(Client::new(Box::new(writer)));
    let samples_consumer: Arc<Mutex<Option<SamplesConsumer>>> = Arc::default();
    let shutdown = Arc::new(AtomicBool::new(false));

    let sink_app = app.clone();
    let sink_consumer = samples_consumer.clone();
    let mut pump = client.pump(Box::new(move |event| match event {
        StreamEvent::Log(log) => {
            let _ = sink_app.emit("log", LogEvent { text: log.text });
        }
        StreamEvent::Telemetry(telemetry) => {
            let _ = sink_app.emit("telemetry", TelemetryEvent::from(telemetry));
        }
        StreamEvent::Samples(samples) => {
            if let Ok(consumer) = sink_consumer.lock() {
                if let Some(consumer) = consumer.as_ref() {
                    consumer(samples);
                }
            }
        }
    }));

    let reader_shutdown = shutdown.clone();
    let reader_app = app.clone();
    let mut reader_port = opened;
    let reader = std::thread::Builder::new()
        .name("session-reader".into())
        .spawn(move || {
            let mut buf = [0u8; 1024];
            let mut consecutive_errors = 0u32;
            while !reader_shutdown.load(Ordering::Relaxed) {
                let n = match reader_port.read(&mut buf) {
                    Ok(n) => {
                        consecutive_errors = 0;
                        n
                    }
                    Err(e) if e.kind() == std::io::ErrorKind::TimedOut => {
                        consecutive_errors = 0;
                        0
                    }
                    Err(_) => {
                        // A vanished port (reset/unplug) errors every read; a
                        // run of them means the port is gone, not a glitch.
                        consecutive_errors += 1;
                        if consecutive_errors >= 5 {
                            let _ = reader_app.emit(
                                "connection",
                                ConnectionEvent {
                                    state: "lost",
                                    port: None,
                                    build_id: None,
                                },
                            );
                            break;
                        }
                        std::thread::sleep(Duration::from_millis(20));
                        0
                    }
                };
                pump.push(&buf[..n]);
            }
        })
        .map_err(|e| format!("spawn reader: {e}"))?;

    let mut session = Session {
        client,
        port_name: port.clone(),
        device_build_id: String::new(),
        samples_consumer,
        shutdown,
        reader: Some(reader),
    };

    let identity = session
        .client
        .request(pcs_proto::shared::envelope::Payload::IdentityRequest(
            pcs_proto::shared::IdentityRequest::default(),
        ));
    let build_id = match identity {
        Ok(pcs_proto::shared::envelope::Payload::Identity(identity)) => identity.build_id,
        Ok(other) => {
            stop_reader(&mut session);
            return Err(format!("unexpected reply: {other:?}"));
        }
        Err(e) => {
            stop_reader(&mut session);
            return Err(e);
        }
    };
    session.device_build_id = build_id.clone();

    let _ = app.emit(
        "connection",
        ConnectionEvent {
            state: "connected",
            port: Some(port),
            build_id: Some(build_id.clone()),
        },
    );
    *state.0.lock().map_err(|_| "session state poisoned")? = Some(session);
    Ok(build_id)
}

#[tauri::command]
pub fn disconnect(app: AppHandle, state: State<SessionState>) {
    if teardown(&state) {
        let _ = app.emit(
            "connection",
            ConnectionEvent {
                state: "disconnected",
                port: None,
                build_id: None,
            },
        );
    }
}

// [impl->app~arch_001~1]
#[tauri::command]
pub fn get_status(state: State<SessionState>) -> SessionStatus {
    match state.0.lock().ok().as_deref() {
        Some(Some(session)) => SessionStatus {
            connected: true,
            port: Some(session.port_name.clone()),
            device_build_id: Some(session.device_build_id.clone()),
        },
        _ => SessionStatus {
            connected: false,
            port: None,
            device_build_id: None,
        },
    }
}

/// Stop the reader and drop the session; true if one existed. The port closes
/// with the drop, which lowers DTR — the board clears its watch list itself.
fn teardown(state: &State<SessionState>) -> bool {
    let session = state.0.lock().ok().and_then(|mut s| s.take());
    match session {
        Some(mut session) => {
            stop_reader(&mut session);
            true
        }
        None => false,
    }
}

fn stop_reader(session: &mut Session) {
    session.shutdown.store(true, Ordering::Relaxed);
    if let Some(handle) = session.reader.take() {
        let _ = handle.join();
    }
}
