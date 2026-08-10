//! MF4 trace drops for the sanity suite.
//!
//! Gated by `PCS_SIL_TRACE_DIR`: when set, a check that owns an [`Engine`] serializes
//! its historian and spawns the repo venv's `tools/mf4_build.py` (asammdf) to write
//! `<dir>/<name>.mf4`. Unset → [`maybe_dump`] is a no-op (zero cost, no behavior
//! change). Any failure (no venv, spawn error, nonzero exit) degrades to writing the
//! raw `<dir>/<name>.bin` stream next to the target — data is never lost — and prints
//! a warning; the suite never aborts on a trace failure.

use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use voyant::Engine;

/// The env var that gates + directs trace drops (the output directory).
const TRACE_DIR_ENV: &str = "PCS_SIL_TRACE_DIR";

/// Drop `<PCS_SIL_TRACE_DIR>/<name>.mf4` from the engine's historian, if the env var
/// is set; a no-op otherwise. Warnings print but never abort the suite.
pub fn maybe_dump(eng: &Engine, name: &str) {
    let Some(dir_env) = std::env::var_os(TRACE_DIR_ENV) else {
        return;
    };
    let root = repo_root();
    let dir = root.join(dir_env);
    if let Err(e) = std::fs::create_dir_all(&dir) {
        eprintln!("         trace: cannot create {}: {e}", dir.display());
        return;
    }
    let out = dir.join(format!("{name}.mf4"));
    match dump_mf4(eng, &root, &out) {
        Ok(()) => println!("         trace -> {}", out.display()),
        Err(e) => eprintln!("         trace: {e}"),
    }
}

/// Serialize the historian and pipe it to the venv's `mf4_build.py` to write
/// `out_path`. On any failure, write the raw stream to `out_path` with a `.bin`
/// extension and return a describing error (the caller only warns — data survives).
fn dump_mf4(eng: &Engine, root: &Path, out_path: &Path) -> std::io::Result<()> {
    // Serialize once into memory — the pipe payload, and the fallback payload.
    let mut buf: Vec<u8> = Vec::new();
    eng.dump_trace(&mut buf, None)?;

    if let Some(py) = venv_python(root) {
        let script = root.join("tools/mf4_build.py");
        let built = pipe_to_builder(&py, &script, out_path, &buf);
        if built {
            return Ok(());
        }
    }

    // Fallback: never lose data.
    let bin = out_path.with_extension("bin");
    std::fs::write(&bin, &buf)?;
    Err(std::io::Error::other(format!(
        "mf4 build unavailable (no venv/asammdf, or builder failed); wrote raw stream to {}",
        bin.display()
    )))
}

/// Spawn `<py> <script> --out <out_path>`, feed the stream on stdin, and report
/// success (spawned, stdin written, exited zero). Any hiccup returns `false`.
fn pipe_to_builder(py: &Path, script: &Path, out_path: &Path, buf: &[u8]) -> bool {
    let Ok(mut child) = Command::new(py)
        .arg(script)
        .arg("--out")
        .arg(out_path)
        .stdin(Stdio::piped())
        .spawn()
    else {
        return false;
    };
    // Write + drop stdin so the child sees EOF, then wait for its exit code.
    if let Some(mut stdin) = child.stdin.take() {
        if stdin.write_all(buf).is_err() {
            return false;
        }
    }
    matches!(child.wait(), Ok(status) if status.success())
}

/// The repo venv's python, if it exists.
fn venv_python(root: &Path) -> Option<PathBuf> {
    let rel = if cfg!(windows) {
        ".venv/Scripts/python.exe"
    } else {
        ".venv/bin/python"
    };
    let py = root.join(rel);
    py.exists().then_some(py)
}

/// Walk up from the current dir to the repo root — the directory holding `setup.sh`
/// and `tools/`, the anchor for the venv and the builder script. Falls back to the
/// current dir if no marker turns up.
fn repo_root() -> PathBuf {
    let cwd = std::env::current_dir().unwrap_or_else(|_| PathBuf::from("."));
    let mut dir = cwd.as_path();
    loop {
        if dir.join("setup.sh").is_file() && dir.join("tools").is_dir() {
            return dir.to_path_buf();
        }
        match dir.parent() {
            Some(parent) => dir = parent,
            None => return cwd,
        }
    }
}
