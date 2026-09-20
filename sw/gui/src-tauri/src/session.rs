//! The native core's device session: the serial port, reader thread, and
//! protocol state live here; the webview only presents the command results
//! and the "connection" / "log" / "telemetry" events.
//! (app~arch_001: a UI reload must not disturb this state.)

use std::io::Read;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;
use std::time::Duration;

use serialport::SerialPort;

use tauri::{AppHandle, Emitter, Manager, State};

use crate::emitter::{UiEmitter, UiEvent};
use crate::protocol::{Client, StreamEvent};

/// Trace-client seam: once installed, receives every raw `Samples` stream
/// message. Demultiplexing is its owner's concern, not the session's.
pub type SamplesConsumer = Arc<dyn Fn(pcs_proto::trace::Samples) + Send + Sync>;

pub struct Session {
    pub client: Arc<Client>,
    port_name: String,
    device_build_id: String,
    samples_consumer: Arc<Mutex<Option<SamplesConsumer>>>,
    ui: Arc<UiEmitter>,
    shutdown: Arc<AtomicBool>,
    reader: Option<JoinHandle<()>>,
}

impl Session {
    /// Install the trace client's raw-Samples consumer (replacing any prior).
    pub fn set_samples_consumer(&self, consumer: Option<SamplesConsumer>) {
        *self
            .samples_consumer
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner) = consumer;
    }

    /// The build identity the device reported at connect.
    pub fn device_build_id(&self) -> &str {
        &self.device_build_id
    }

    /// This session's UI emitter: the only path from a reader thread to the
    /// webview.
    pub fn ui_emitter(&self) -> Arc<UiEmitter> {
        self.ui.clone()
    }

    fn reader_alive(&self) -> bool {
        self.reader.as_ref().is_some_and(|h| !h.is_finished())
    }
}

impl Drop for Session {
    /// Every drop path stops the reader and closes the port; the close lowers
    /// DTR, on which the board clears its watch list itself. The emitter stops
    /// with it, so a dead session's queued events never reach the next one.
    fn drop(&mut self) {
        self.shutdown.store(true, Ordering::Relaxed);
        self.ui.shut_down();
        if let Some(handle) = self.reader.take() {
            let _ = handle.join();
        }
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
pub struct ConnectionEvent {
    pub state: &'static str,
    port: Option<String>,
    build_id: Option<String>,
}

impl ConnectionEvent {
    pub fn down(state: &'static str) -> Self {
        Self {
            state,
            port: None,
            build_id: None,
        }
    }
}

#[derive(Clone, serde::Serialize)]
pub struct LogEvent {
    pub text: String,
}

#[derive(Clone, serde::Serialize)]
pub struct TelemetryEvent {
    pub timestamp_ms: u32,
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

/// macOS lists every serial device twice: a `/dev/tty.*` call-in node and
/// its `/dev/cu.*` call-out twin. The app dials out, so a tty entry whose cu
/// twin is listed is noise; the `/dev/tty.` prefix only occurs on macOS, so
/// the rule is a structural no-op elsewhere.
fn is_shadowed_tty_twin(name: &str, all: &[String]) -> bool {
    match name.strip_prefix("/dev/tty.") {
        Some(suffix) => all
            .iter()
            .any(|n| n.strip_prefix("/dev/cu.") == Some(suffix)),
        None => false,
    }
}

// [impl->app~conn_001~1]
#[tauri::command]
pub fn list_ports() -> Vec<PortInfo> {
    let ports = serialport::available_ports().unwrap_or_else(|e| {
        eprintln!("list ports: {e}");
        Vec::new()
    });
    let names: Vec<String> = ports.iter().map(|p| p.port_name.clone()).collect();
    ports
        .into_iter()
        .filter(|p| !is_shadowed_tty_twin(&p.port_name, &names))
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
/// returns the reported build id. Blocks on the port open and the identity
/// round trip, so it runs off the main thread.
// [impl->app~conn_001~1]
#[tauri::command]
pub async fn connect(app: AppHandle, port: String) -> Result<String, String> {
    tauri::async_runtime::spawn_blocking(move || {
        let state = app.state::<SessionState>();
        let trace = app.state::<crate::trace::TraceState>();
        connect_blocking(&app, &state, &trace, port)
    })
    .await
    .map_err(|e| format!("connect: {e}"))?
}

fn connect_blocking(
    app: &AppHandle,
    state: &SessionState,
    trace: &crate::trace::TraceState,
    port: String,
) -> Result<String, String> {
    // A fresh connect replaces any existing session whole; a dead session's
    // residual trace points must not leak into the new one.
    teardown(state);
    crate::trace::drop_session_state(trace);

    let mut opened = serialport::new(&port, 115_200)
        .timeout(Duration::from_millis(50))
        .open_native()
        .map_err(|e| format!("open {port}: {e}"))?;
    grow_driver_queue(&opened);
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

    // Every emit crosses the emitter thread: a webview emit needs the main
    // thread, and a reader blocked on one stops parsing the replies queued
    // behind it.
    let emit_app = app.clone();
    let ui = Arc::new(UiEmitter::new(Box::new(move |event| match event {
        UiEvent::Samples(batch) => {
            let _ = emit_app.emit("samples", batch);
        }
        UiEvent::Telemetry(telemetry) => {
            let _ = emit_app.emit("telemetry", telemetry);
        }
        UiEvent::Log(log) => {
            let _ = emit_app.emit("log", log);
        }
        UiEvent::Connection(connection) => {
            let _ = emit_app.emit("connection", connection);
        }
        UiEvent::TraceStatus(status) => {
            let _ = emit_app.emit("trace-status", status);
        }
    }))?);

    let sink_ui = ui.clone();
    let sink_consumer = samples_consumer.clone();
    let mut pump = client.pump(Box::new(move |event| match event {
        StreamEvent::Log(log) => {
            sink_ui.send(UiEvent::Log(LogEvent { text: log.text }));
        }
        StreamEvent::Telemetry(telemetry) => {
            sink_ui.send(UiEvent::Telemetry(TelemetryEvent::from(telemetry)));
        }
        StreamEvent::Samples(samples) => {
            // Clone the Arc out and call unlocked: a slow emit must not
            // block install/clear on the consumer slot.
            let consumer = sink_consumer.lock().ok().and_then(|g| (*g).clone());
            if let Some(consumer) = consumer {
                consumer(samples);
            }
        }
    }));

    let reader_shutdown = shutdown.clone();
    let reader_ui = ui.clone();
    let mut reader_port = opened;
    let reader = std::thread::Builder::new()
        .name("session-reader".into())
        .spawn(move || {
            let mut buf = [0u8; 4096];
            let mut consecutive_errors = 0u32;
            let mut lost = false;
            while !reader_shutdown.load(Ordering::Relaxed) {
                let n = match reader_port.read(&mut buf) {
                    Ok(n) if n > 0 => {
                        consecutive_errors = 0;
                        n
                    }
                    Err(e) if e.kind() == std::io::ErrorKind::TimedOut => {
                        consecutive_errors = 0;
                        0
                    }
                    // A vanished port errors (or EOFs — Ok(0) is never a
                    // healthy idle; that arrives as TimedOut) every read; a
                    // run of them means the port is gone, not a glitch.
                    _ => {
                        consecutive_errors += 1;
                        if consecutive_errors >= 5 {
                            lost = true;
                            break;
                        }
                        std::thread::sleep(Duration::from_millis(20));
                        0
                    }
                };
                pump.push(&buf[..n]);
            }
            // Emitted off the read path: a send that waits for room must not
            // hold a reader that `Session::drop` is joining.
            if lost {
                reader_ui.send(UiEvent::Connection(ConnectionEvent::down("lost")));
            }
        })
        .map_err(|e| format!("spawn reader: {e}"))?;

    let mut session = Session {
        client,
        port_name: port.clone(),
        device_build_id: String::new(),
        samples_consumer,
        ui,
        shutdown,
        reader: Some(reader),
    };

    let identity = session
        .client
        .request(pcs_proto::shared::envelope::Payload::IdentityRequest(
            pcs_proto::shared::IdentityRequest::default(),
        ));
    // On any error, dropping `session` stops the reader and closes the port.
    let build_id = match identity {
        Ok(pcs_proto::shared::envelope::Payload::Identity(identity)) => identity.build_id,
        Ok(other) => {
            return Err(format!(
                "unexpected reply: {}",
                crate::protocol::payload_kind(&other)
            ))
        }
        Err(e) => return Err(e),
    };
    session.device_build_id = build_id.clone();

    // Installed before the edge goes out: a UI that reacts to "connected" by
    // querying the session must find it there.
    *state.0.lock().map_err(|_| "session state poisoned")? = Some(session);
    let _ = app.emit(
        "connection",
        ConnectionEvent {
            state: "connected",
            port: Some(port),
            build_id: Some(build_id.clone()),
        },
    );
    Ok(build_id)
}

/// Joins the reader thread and closes the port, so it runs off the main
/// thread. The session's emitter dies with it; this edge is emitted here.
#[tauri::command]
pub async fn disconnect(app: AppHandle) {
    let joined = tauri::async_runtime::spawn_blocking(move || {
        crate::trace::drop_session_state(&app.state::<crate::trace::TraceState>());
        if teardown(&app.state::<SessionState>()) {
            let _ = app.emit("connection", ConnectionEvent::down("disconnected"));
        }
    })
    .await;
    if let Err(e) = joined {
        eprintln!("disconnect: {e}");
    }
}

// [impl->app~arch_001~1]
#[tauri::command]
pub fn get_status(state: State<SessionState>) -> SessionStatus {
    match state.0.lock().ok().as_deref() {
        // A dead reader means the link was lost even though the session
        // lingers — a UI reload must not restore it as connected.
        Some(Some(session)) if session.reader_alive() => SessionStatus {
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

/// Windows usbser keeps a 4 KB receive queue by default; a few ms of host
/// latency at hundreds of kB/s overruns it and corrupts frames. A driver that
/// refuses 1 MB gets one retry at 64 KB — still far above the default.
#[cfg(windows)]
fn grow_driver_queue(port: &serialport::COMPort) {
    use std::os::windows::io::AsRawHandle;
    #[link(name = "kernel32")]
    extern "system" {
        fn SetupComm(handle: *mut std::ffi::c_void, in_queue: u32, out_queue: u32) -> i32;
    }
    // SAFETY: a valid open handle; SetupComm only resizes the driver queues.
    let grown = unsafe {
        (SetupComm(port.as_raw_handle(), 1 << 20, 1 << 14) != 0)
            || (SetupComm(port.as_raw_handle(), 1 << 16, 1 << 14) != 0)
    };
    if !grown {
        eprintln!("SetupComm: driver kept its default receive queue; frames may drop");
    }
}

#[cfg(not(windows))]
fn grow_driver_queue(_port: &serialport::TTYPort) {}

/// Drop any existing session (its `Drop` stops the reader); true if one existed.
fn teardown(state: &SessionState) -> bool {
    // The guard is released at the end of this statement, so the reader join
    // in `Session::drop` runs unlocked — `get_status` is synchronous and
    // would otherwise block on it.
    let session = state
        .0
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
        .take();
    session.is_some()
}

#[cfg(test)]
mod tests {
    use super::is_shadowed_tty_twin;

    fn names(list: &[&str]) -> Vec<String> {
        list.iter().map(|s| s.to_string()).collect()
    }

    #[test]
    fn tty_twin_is_dropped_when_its_cu_sibling_is_listed() {
        let all = names(&[
            "/dev/cu.usbmodem1101",
            "/dev/tty.usbmodem1101",
            "/dev/cu.Bluetooth-Incoming-Port",
            "/dev/tty.Bluetooth-Incoming-Port",
        ]);
        let kept: Vec<&String> = all
            .iter()
            .filter(|n| !is_shadowed_tty_twin(n, &all))
            .collect();
        assert_eq!(
            kept,
            [&all[0], &all[2]],
            "only the cu.* call-out nodes survive"
        );
    }

    #[test]
    fn tty_without_a_twin_passes_through() {
        let all = names(&["/dev/tty.orphanmodem", "/dev/cu.other"]);
        assert!(!is_shadowed_tty_twin(&all[0], &all));
    }

    #[test]
    fn non_mac_names_never_match() {
        let all = names(&["COM8", "/dev/ttyUSB0", "/dev/ttyACM0"]);
        for n in &all {
            assert!(!is_shadowed_tty_twin(n, &all), "{n} must pass through");
        }
    }
}
