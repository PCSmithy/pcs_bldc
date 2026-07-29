//! pcs_bldc SIL harness — the instantiation's test-facing surface.
//!
//! Each scenario is an independent `#[test]` in `tests/*.rs` over a fresh [`Sil`]
//! world: [`Sil::new`] loads this board's firmware DLL, boots it from reset, and
//! exposes the simulation as [`Sil::sim`]; on drop it dumps a per-test `.mf4` trace
//! (when `PCS_SIL_TRACE_DIR` is set) and unloads the library so the next `Sil::new`
//! boots statics from scratch. A process-global mutex serializes vanilla (threaded)
//! `cargo test`; under cargo-nextest each test is its own process and the mutex is
//! uncontended — the harness is nextest-compatible by construction.
//!
//! Board models and helpers ([`As5048Model`], [`trace`], [`TICK_US`], the DLL path
//! resolution) live here so both the tests and the perf bin share one surface.

pub mod as5048;
pub mod models;
mod sil;
pub mod trace;

pub use as5048::As5048Model;
pub use models::CountsRampModel;
pub use sil::{lock_world, Sil};

use std::path::{Path, PathBuf};
use voyant::SignalId;

/// The firmware's 1 ms task cadence — one engine tick per this many µs of sim time.
pub const TICK_US: u64 = 1_000;

/// The `<source>` segment of this board's firmware signals (`cvar:pcs_bldc:…`).
pub const SOURCE: &str = "pcs_bldc";

/// The host shared-library file name for this platform.
const LIBNAME: &str = if cfg!(target_os = "windows") {
    "libpcs_bldc_fw.dll"
} else if cfg!(target_os = "macos") {
    "libpcs_bldc_fw.dylib"
} else {
    "libpcs_bldc_fw.so"
};

/// Resolve the firmware shared library: the `PCS_SIL_DLL` override if set, else the
/// build-tree default matching the Rust profile (`build/native-fw[-release]/src`,
/// relative to the repo root). The path is not checked here — [`Sil::new`] panics
/// with build guidance if it is missing.
pub fn dll_path() -> PathBuf {
    if let Some(p) = std::env::var_os("PCS_SIL_DLL") {
        return PathBuf::from(p);
    }
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../..");
    let flavor = if cfg!(debug_assertions) {
        "native-fw"
    } else {
        "native-fw-release"
    };
    root.join("build").join(flavor).join("src").join(LIBNAME)
}

/// A `cvar:pcs_bldc:<path>` id for this board's firmware statics.
pub fn cvar(path: &str) -> SignalId {
    SignalId::new("cvar", SOURCE, path, None).expect("valid cvar id")
}

/// The `cvar:pcs_bldc:<path>` id **string** — for the string-keyed engine API
/// (`sim.write`/`sim.read`), which parses the id itself.
pub fn cid(path: &str) -> String {
    format!("cvar:{SOURCE}:{path}")
}
