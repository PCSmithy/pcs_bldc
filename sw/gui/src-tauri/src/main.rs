#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod session;

use session::SessionState;

fn main() {
    tauri::Builder::default()
        .manage(SessionState::default())
        .invoke_handler(tauri::generate_handler![
            session::list_ports,
            session::connect,
            session::disconnect
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
