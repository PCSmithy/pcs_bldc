#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod config;
mod firmware;
mod protocol;
mod session;
mod trace;

use firmware::FirmwareState;
use session::SessionState;
use trace::TraceState;

/// UI zoom (chrome ergonomics): the native webview zoom factor — WebView2's
/// ZoomFactor on Windows, WKWebView page zoom on macOS — driven by the
/// frontend's Ctrl+'+'/'-'/'0' bindings, which also own clamping and
/// persistence.
#[tauri::command]
fn set_zoom(window: tauri::WebviewWindow, factor: f64) -> Result<(), String> {
    window.set_zoom(factor).map_err(|e| e.to_string())
}

fn main() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .manage(SessionState::default())
        .manage(FirmwareState::default())
        .manage(TraceState::default())
        .invoke_handler(tauri::generate_handler![
            session::list_ports,
            session::connect,
            session::disconnect,
            session::get_status,
            firmware::load_elf,
            firmware::list_signals,
            trace::install_watches,
            trace::clear_watches,
            trace::trace_status,
            config::load_config,
            config::save_config,
            set_zoom
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}

// Lives here, not in firmware.rs: that module is also compiled by the
// integration-test harness, whose binary fails to load when serde_json is
// referenced under cfg(test) (STATUS_ENTRYPOINT_NOT_FOUND on Windows).
#[cfg(test)]
mod tests {
    use crate::firmware::SignalInfo;

    /// Pins the serialization contract the frontend consumes: enums as a
    /// [[value, "name"], ...] array, absent entirely for non-enum kinds.
    #[test]
    fn signal_info_serializes_enums_as_value_name_pairs() {
        let info = SignalInfo {
            path: "mode".to_string(),
            kind: "enum".to_string(),
            size: 1,
            readonly: false,
            enums: Some(vec![(0, "IDLE".to_string()), (255, "ERR".to_string())]),
        };
        assert_eq!(
            serde_json::to_string(&info).unwrap(),
            r#"{"path":"mode","kind":"enum","size":1,"readonly":false,"enums":[[0,"IDLE"],[255,"ERR"]]}"#
        );
        let plain = SignalInfo {
            path: "n".to_string(),
            kind: "u32".to_string(),
            size: 4,
            readonly: false,
            enums: None,
        };
        assert_eq!(
            serde_json::to_string(&plain).unwrap(),
            r#"{"path":"n","kind":"u32","size":4,"readonly":false}"#
        );
    }
}
