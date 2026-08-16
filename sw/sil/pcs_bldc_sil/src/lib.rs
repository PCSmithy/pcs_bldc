//! pcs_bldc SIL harness — the instantiation's test-facing surface.
//!
//! Each scenario is an independent `#[test]` in `tests/*.rs` over a [`Sil`] (see its
//! docs): the simulation itself, which derefs to its [`Engine`]. [`board`] assembles
//! the whole-board world — plant, encoders, sense front end, firmware, every route
//! live — and the models, wiring and helpers around it live here so the tests and the
//! perf bin share one surface.

pub mod as5048;
pub mod board;
pub mod current_sense;
pub mod models;
pub mod motor;
mod sil;
pub mod trace;
pub mod wire;
pub mod wiring;

pub use as5048::{decode_frame, As5048Model};
pub use board::{board, Board};
pub use current_sense::{CurrentSenseModel, CurrentSenseParams};
pub use models::CountsRampModel;
pub use motor::{MotorModel, MotorParams};
pub use sil::{lock_world, Sil};
pub use wiring::{wire_bridge, wire_current_sense, BRIDGE_PORTS};

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

/// The `vsig:<source>:<local>` id **string**, for the same string-keyed API.
pub fn vid(source: &str, local: &str) -> String {
    format!("vsig:{source}:{local}")
}
