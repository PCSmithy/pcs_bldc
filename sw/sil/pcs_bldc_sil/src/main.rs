//! pcs_bldc SIL performance bin.
//!
//! Boots this board's firmware once and prints the phase-isolated per-tick
//! performance report. The behavioral checks live in `tests/` (run via
//! `cargo test` — see the crate lib docs); this bin is just the perf instrument,
//! plus an optional white-box per-tick scheduling table under `PCS_SIL_DIAG=1`.
//!
//! Usage: `cargo run -p pcs_bldc_sil -- [path-to-firmware-shared-lib]`
//!
//! Env vars:
//! - `PCS_SIL_DLL` / argv[1] — the firmware library (else the build-tree default).
//! - `PCS_SIL_DLL_FLAVOR` — label printed in the performance report.
//! - `PCS_SIL_DIAG=1` — after boot, print a white-box per-tick scheduling table
//!   (FreeRTOS `xTickCount`, `xNextTaskUnblockTime`, and the per-task heartbeat
//!   counters, all read by DWARF straight from firmware memory).

use pcs_bldc_sil::{dll_path, CountsRampModel, SOURCE, TICK_US};
use std::path::PathBuf;
use std::process::ExitCode;
use std::rc::Rc;
use std::time::Instant;
use voyant::{vsig_id, Engine, Firmware, FirmwareMember, Member, SignalId, StateTable};

fn main() -> ExitCode {
    let path = std::env::args()
        .nth(1)
        .map(PathBuf::from)
        .unwrap_or_else(dll_path);

    println!("=== pcs_bldc SIL performance report ===");
    println!("loading firmware: {}", path.display());
    let fw = match Firmware::load(&path) {
        Ok(f) => Rc::new(f),
        Err(e) => {
            eprintln!("FATAL: failed to load firmware library: {e}");
            return ExitCode::FAILURE;
        }
    };
    if !fw.start() {
        eprintln!("FATAL: sil_fw_start() returned false");
        return ExitCode::FAILURE;
    }

    // Optional white-box diagnostic (PCS_SIL_DIAG=1); print-only, advances the handle.
    diag_per_tick_table(&fw);

    report_performance(&fw);

    fw.shutdown();
    ExitCode::SUCCESS
}

/// White-box per-tick scheduling table, gated by `PCS_SIL_DIAG=1` (no-op otherwise).
/// Straight after boot, advance the firmware one raw tick at a time and, each tick,
/// DWARF-read the FreeRTOS kernel's own view — `xTickCount` and `xNextTaskUnblockTime`
/// (the earliest tick any blocked task is scheduled to wake) — alongside the per-task
/// heartbeat counters. A general-purpose remote debugger for scheduling /
/// DWARF-resolution questions; the resolved-address list surfaces per-variable DWARF
/// faults directly (two distinct statics collapsing to one address means their reads
/// alias). Reads are panic-guarded so a firmware built without a given static degrades
/// that column to `n/a` rather than aborting.
fn diag_per_tick_table(fw: &Firmware) {
    if std::env::var("PCS_SIL_DIAG").ok().as_deref() != Some("1") {
        return;
    }
    const TICKS: u32 = 15;

    // Silence the default panic hook for the duration: a missing DWARF symbol makes
    // `read_cvar` panic, which we catch below and render as "n/a".
    let prev_hook = std::panic::take_hook();
    std::panic::set_hook(Box::new(|_| {}));
    let rd = |path: &str| -> String {
        std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| fw.read_cvar(path)))
            .ok()
            .and_then(|v| v.as_u64())
            .map(|n| n.to_string())
            .unwrap_or_else(|| "n/a".into())
    };

    println!("\n-- DIAG: per-tick firmware scheduling table (PCS_SIL_DIAG=1) --");
    println!(
        "         DLL flavor: {}",
        std::env::var("PCS_SIL_DLL_FLAVOR").unwrap_or_else(|_| "unknown".into())
    );

    // Resolved runtime addresses of the statics the table reads. If two distinct
    // statics collapse to one address here, their reads alias — a per-variable
    // DWARF-resolution fault (e.g. same-TU file-scope `static`s on Mach-O).
    println!("         resolved addresses (watch for collisions):");
    for p in [
        "xTickCount",
        "task1msRuns",
        "task10msRuns",
        "telemRuns",
        "task200msRuns",
        "taskUsbRuns",
    ] {
        let a = fw
            .resolve_addr(p)
            .map(|a| format!("{a:#018x}"))
            .unwrap_or_else(|| "n/a".into());
        println!("           {p:<16} {a}");
    }

    println!(
        "         {:>4}  {:>10}  {:>9}  {:>7}  {:>8}  {:>5}  {:>7}",
        "tick", "xTickCount", "nextUnblk", "task1ms", "task10ms", "telem", "taskUsb"
    );
    // Row 0 = post-boot baseline (all tasks just blocked; no tick applied yet).
    for i in 0..=TICKS {
        println!(
            "         {:>4}  {:>10}  {:>9}  {:>7}  {:>8}  {:>5}  {:>7}",
            i,
            rd("xTickCount"),
            rd("xNextTaskUnblockTime"),
            rd("task1msRuns"),
            rd("task10msRuns"),
            rd("telemRuns"),
            rd("taskUsbRuns")
        );
        if i < TICKS {
            fw.advance_tick();
        }
    }

    std::panic::set_hook(prev_hook);
}

/// Phase-isolated performance report (informational). Times each phase over `N` ticks
/// after a warm-up and prints µs/tick + ×realtime. `std::time` is fine here — driver
/// code, not sim-deterministic state. The phases isolate the cost:
///   1. firmware `advance_tick()` alone (no engine machinery);
///   2. full engine `step()` (a model + a `FirmwareMember` + a route);
///   3. firmware-member-only step (the sweep+flush floor);
///   4. empty engine `step()` (the engine's floor);
///
/// plus the derived splits. The report names the Rust profile and DLL flavor
/// (`PCS_SIL_DLL_FLAVOR`) so a copied-out table is self-describing.
fn report_performance(fw: &Rc<Firmware>) {
    const WARMUP: u64 = 100;
    const N: u64 = 1000;

    // Registered-leaf count: enable a throwaway member on a scratch table.
    let leaves = {
        let mut st = StateTable::new();
        let mut fwm = FirmwareMember::new(SOURCE, Rc::clone(fw), TICK_US);
        fwm.set_enabled(true, &mut st);
        fwm.cvar_leaf_count()
    };

    // (1) Firmware tick alone — raw advance_tick loop, no engine machinery.
    let fw_us = time_avg_us(WARMUP, N, || fw.advance_tick());

    // (2) Full engine step — a model drives a firmware input cvar through a route
    //     while the FirmwareMember mirrors the whole namespace each tick.
    let full_us = {
        const STEP: u32 = 25; // stays within the u8 destination byte
        let src = vsig_id("sensor", "counts").expect("valid vsig id");
        let dst =
            SignalId::new("cvar", SOURCE, "HW_USB_sim_data.rx[0]", None).expect("valid cvar id");
        let mut eng = Engine::new(TICK_US);
        eng.add_member(CountsRampModel::new("sensor", STEP));
        let mut fwm = FirmwareMember::new(SOURCE, Rc::clone(fw), TICK_US);
        fwm.register_cvar_in_state_table("HW_USB_sim_data.rx[0]");
        eng.add_member(fwm);
        eng.add_route(src, dst).expect("add route");
        time_avg_us(WARMUP, N, || {
            eng.step().expect("engine step");
        })
    };

    // (3) Firmware-member-only step — a lone FirmwareMember mirroring the whole
    //     namespace (no model, no route): isolates the Tier-1 shadow sweep + flush.
    let sweep_us = {
        let mut eng = Engine::new(TICK_US);
        eng.add_member(FirmwareMember::new(SOURCE, Rc::clone(fw), TICK_US));
        time_avg_us(WARMUP, N, || {
            eng.step().expect("engine step");
        })
    };

    // (4) Empty engine floor — no members, no routes: the engine's own per-step cost.
    let floor_us = {
        let mut eng = Engine::new(TICK_US);
        time_avg_us(WARMUP, N, || {
            eng.step().expect("engine step");
        })
    };

    // (5) Derived — everything the full step adds over a bare firmware tick.
    let derived_us = full_us - fw_us;
    let sweep_over_fw_us = sweep_us - fw_us;
    let model_route_us = full_us - sweep_us;

    let rust_profile = if cfg!(debug_assertions) {
        "debug"
    } else {
        "release"
    };
    let dll_flavor = std::env::var("PCS_SIL_DLL_FLAVOR")
        .unwrap_or_else(|_| "unknown (build via tools/run_sil.sh)".into());
    // ×realtime = sim-time-per-tick / wall-time-per-tick = TICK_US / (µs/tick).
    let xrt = |us: f64| (TICK_US as f64) / us;

    println!("\n-- performance report (phase-isolated, informational) --");
    println!("         Rust profile: {rust_profile}    firmware DLL: {dll_flavor}");
    println!("         {leaves} cvar leaves mirrored/tick    (sim tick = {TICK_US} µs, avg over {N} ticks)");
    println!("         phase                              µs/tick   ×realtime");
    println!(
        "         firmware advance_tick alone        {fw_us:>7.2}   {:>6.1}×",
        xrt(fw_us)
    );
    println!(
        "         full engine step (measured)        {full_us:>7.2}   {:>6.1}×",
        xrt(full_us)
    );
    println!(
        "         firmware-member step (sweep+flush) {sweep_us:>7.2}   {:>6.1}×",
        xrt(sweep_us)
    );
    println!("         empty engine step (floor)          {floor_us:>7.2}");
    println!("         derived: full - firmware           {derived_us:>7.2}   (sweep+flush+ports+routes+table)");
    println!("           of which shadow sweep+flush      {sweep_over_fw_us:>7.2}   (member step - firmware)");
    println!("           of which model+route+propagate   {model_route_us:>7.2}   (full step - member step)");
}

/// Warm up `body` for `warmup` iterations, then time `n` more and return the mean
/// wall-clock µs per iteration. Driver-side only (voyant core is wall-clock-free).
fn time_avg_us(warmup: u64, n: u64, mut body: impl FnMut()) -> f64 {
    for _ in 0..warmup {
        body();
    }
    let start = Instant::now();
    for _ in 0..n {
        body();
    }
    (start.elapsed().as_nanos() as f64) / (n as f64) / 1000.0
}
