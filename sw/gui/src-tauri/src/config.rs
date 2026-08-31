//! Session-config persistence: one JSON file in the app config dir, read
//! whole at boot and written whole (atomically) as state changes. The
//! frontend owns the schema; the core only stores it.
//! (app~arch_002: the session context this file carries is what launch
//! restores.)

use std::fs;
use std::path::PathBuf;

use tauri::Manager;

const CONFIG_FILE: &str = "cockpit.json";

fn config_path(app: &tauri::AppHandle) -> Result<PathBuf, String> {
    let dir = app
        .path()
        .app_config_dir()
        .map_err(|e| format!("resolve config dir: {e}"))?;
    fs::create_dir_all(&dir).map_err(|e| format!("create {}: {e}", dir.display()))?;
    Ok(dir.join(CONFIG_FILE))
}

// [impl->app~arch_002~1] the saved session context's storage
#[tauri::command]
pub fn load_config(app: tauri::AppHandle) -> serde_json::Value {
    let empty = serde_json::json!({});
    let Ok(path) = config_path(&app) else {
        return empty;
    };
    let text = match fs::read_to_string(&path) {
        Ok(text) => text,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return empty,
        Err(e) => {
            // A transient read error must leave a trace before the next save
            // can clobber a good file.
            eprintln!("config {}: {e}", path.display());
            return empty;
        }
    };
    match serde_json::from_str(&text) {
        Ok(value) => value,
        Err(e) => {
            // A corrupt file must never fail the launch: set it aside for
            // inspection and start fresh.
            eprintln!("config {}: {e}; renaming aside", path.display());
            let _ = fs::rename(&path, path.with_extension("json.bad"));
            empty
        }
    }
}

#[tauri::command]
pub fn save_config(app: tauri::AppHandle, value: serde_json::Value) -> Result<(), String> {
    let path = config_path(&app)?;
    let text = serde_json::to_string_pretty(&value).map_err(|e| e.to_string())?;
    // Atomic replace: a crash mid-write leaves the previous file intact.
    let tmp = path.with_extension("json.tmp");
    fs::write(&tmp, text).map_err(|e| format!("write {}: {e}", tmp.display()))?;
    fs::rename(&tmp, &path).map_err(|e| format!("commit {}: {e}", path.display()))
}
