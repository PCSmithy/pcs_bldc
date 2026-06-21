//! Phase-2 proof-of-life driver: Rust loads the firmware shared library, drives
//! it through the control ABI, and reads firmware state live by symbol — the
//! white-box loop, now in Rust (it mirrors the C `LoadLibrary` harness that
//! validated the DLL).
//!
//! Usage: `cargo run -p sil-core -- [path-to-libpcs_bldc_fw.dll]`
//! (defaults to the native build output under build/native-fw/).

use std::path::PathBuf;
use sil_sys::Firmware;

fn default_dll_path() -> PathBuf {
    // Run from the workspace dir (sw/sil); the native build output is here:
    PathBuf::from("../../build/native-fw/src/libpcs_bldc_fw.dll")
}

fn main() {
    let path = std::env::args()
        .nth(1)
        .map(PathBuf::from)
        .unwrap_or_else(default_dll_path);

    println!("loading firmware: {}", path.display());
    let fw = Firmware::load(&path).expect("failed to load firmware DLL");

    assert!(fw.start(), "sil_fw_start() returned false");

    for tick in 1..=20u32 {
        fw.advance_tick();
        let runs = fw.read_u32(b"sim_task1msRuns\0");
        println!("tick {tick:2}  sim_task1msRuns = {runs}  (read from firmware memory)");
    }

    fw.shutdown();
    println!("done — Rust drove the firmware over the control ABI and read its state by symbol.");
}
