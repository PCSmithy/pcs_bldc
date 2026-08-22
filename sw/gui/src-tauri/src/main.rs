#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod config;
mod firmware;
mod protocol;
mod session;
mod trace;

use firmware::FirmwareState;
use session::SessionState;
use trace::TraceState;

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
            config::save_config
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
