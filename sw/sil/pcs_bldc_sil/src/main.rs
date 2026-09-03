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

use pcs_bldc_sil::{dll_path, CountsRampModel, Sil, SOURCE, TICK_US};
use std::path::PathBuf;
use std::process::ExitCode;
use std::rc::Rc;
use std::time::Instant;
use voyant::{
    vsig_id, Engine, Firmware, FirmwareMember, Member, SignalId, StateTable,
    DEFAULT_SWEEP_PERIOD_US,
};

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
    report_fine_grid();
    report_board_world();

    fw.shutdown();
    ExitCode::SUCCESS
}

/// The FreeRTOS port's kernel-tick handler, which it registers with the interrupt
/// table at scheduler start. Driving a raw firmware step outside the engine means
/// advancing the timebase and dispatching this entry by hand.
const SYSTICK_ISR: &str = "vSilSysTickHandler";

/// White-box per-tick scheduling table, gated by `PCS_SIL_DIAG=1` (no-op otherwise):
/// advance the firmware one raw tick at a time and DWARF-read the FreeRTOS kernel's own
/// view alongside the per-task heartbeats. The resolved-address list surfaces DWARF
/// faults directly — two statics on one address means their reads alias.
fn diag_per_tick_table(fw: &Firmware) {
    if std::env::var("PCS_SIL_DIAG").ok().as_deref() != Some("1") {
        return;
    }
    const TICKS: u32 = 15;
    let systick = fw
        .resolve_func(SYSTICK_ISR)
        .expect("the fiber port's kernel-tick handler resolves by name");

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
        "serverRuns",
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
        "tick", "xTickCount", "nextUnblk", "task1ms", "task10ms", "server", "taskUsb"
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
            rd("serverRuns"),
            rd("taskUsbRuns")
        );
        if i < TICKS {
            fw.advance_time(TICK_US);
            fw.dispatch_isr(systick);
        }
    }

    std::panic::set_hook(prev_hook);
}

/// Phase-isolated performance report (informational): time each phase over `N` ticks
/// after a warm-up and print µs/tick + ×realtime, plus the derived splits. The four
/// phases isolate a raw firmware step, a full engine step, a firmware-member-only step
/// (sweep+flush) and an empty engine step (the floor).
fn report_performance(fw: &Rc<Firmware>) {
    const WARMUP: u64 = 100;
    const N: u64 = 1000;

    // Registered-leaf count: enable a throwaway member on a scratch table.
    let leaves = {
        let mut st = StateTable::new();
        let mut fwm = FirmwareMember::new(SOURCE, Rc::clone(fw));
        fwm.set_enabled(true, &mut st);
        fwm.cvar_leaf_count()
    };

    // (1) Firmware step alone — the timebase advance plus the one interrupt a booted
    //     image has due each grid step (its kernel tick), straight over the control
    //     ABI with no engine machinery around it.
    let systick = fw
        .resolve_func(SYSTICK_ISR)
        .expect("the fiber port's kernel-tick handler resolves by name");
    let fw_us = time_avg_us(WARMUP, N, || {
        fw.advance_time(TICK_US);
        fw.dispatch_isr(systick);
    });

    // (2) Full engine step — a model drives a firmware input cvar through a route
    //     while the FirmwareMember mirrors the whole namespace each tick.
    let full_us = {
        const STEP: u32 = 25; // stays within the u8 destination byte
        let src = vsig_id("sensor", "counts").expect("valid vsig id");
        let dst =
            SignalId::new("cvar", SOURCE, "HW_USB_sim_data.rx[0]", None).expect("valid cvar id");
        let mut eng = Engine::new(TICK_US);
        eng.add_member(CountsRampModel::new("sensor", STEP));
        let mut fwm = FirmwareMember::new(SOURCE, Rc::clone(fw));
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
        eng.add_member(FirmwareMember::new(SOURCE, Rc::clone(fw)));
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
        "         firmware step (time+systick)       {fw_us:>7.2}   {:>6.1}×",
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

/// The fine grid a center-aligned control interrupt needs: the 20 kHz PWM period.
const FINE_GRID_US: u64 = 50;
/// The sim USB device handler, stood in as a control-rate interrupt: it notifies a
/// real FreeRTOS task, so a dispatch costs what a handler waking a task costs.
const FINE_ISR: &str = "HW_USB_sim_irqHandler";

/// Fine-grid report: the same members and route as the coarse full-step row, on a
/// 50 µs grid. Three worlds isolate the mirror gate — the firmware's own kernel-rate
/// interrupts, then a control-rate interrupt due on every step with the mirror gated
/// and ungated. Each world loads its own firmware copy, so one world's interrupt
/// registrations never reach the next.
fn report_fine_grid() {
    const WARMUP: u64 = 500;
    const N: u64 = 20_000;

    let plain = time_fine_world(None, DEFAULT_SWEEP_PERIOD_US, WARMUP, N);
    let gated = time_fine_world(Some(FINE_GRID_US), DEFAULT_SWEEP_PERIOD_US, WARMUP, N);
    let ungated = time_fine_world(Some(FINE_GRID_US), 0, WARMUP, N);
    let xrt = |us: f64| (FINE_GRID_US as f64) / us;

    println!("\n-- fine-grid report (informational) --");
    println!("         sim grid = {FINE_GRID_US} µs, mirror cadence = {DEFAULT_SWEEP_PERIOD_US} µs, avg over {N} steps");
    println!("         phase                              µs/step   ×realtime");
    println!(
        "         full step, 1 ms interrupts         {plain:>7.2}   {:>6.1}×",
        xrt(plain)
    );
    println!(
        "         + a {FINE_GRID_US} µs interrupt (gated mirror) {gated:>7.2}   {:>6.1}×",
        xrt(gated)
    );
    println!(
        "         + a {FINE_GRID_US} µs interrupt (no gate)      {ungated:>7.2}   {:>6.1}×",
        xrt(ungated)
    );
    println!(
        "         gate saves                         {:>7.2}   (no gate - gated)",
        ungated - gated
    );
}

/// Board-world member isolation on the control-rate grid: the full step cost,
/// then the same world re-timed with one member disabled at a time — each row's
/// delta approximates that member's per-step share (routes, table, and mirror
/// stay live throughout; interactions are ignored). Measured bridge-dark after
/// gate bring-up; a spinning world adds commutation work on kernel-tick steps.
fn report_board_world() {
    use pcs_bldc_sil::board::{board_with, DIAL, ENCODER, GATE_BRINGUP_MS, MOTOR, SENSE};
    const WARMUP: u64 = 2_000;
    const N: u64 = 20_000;

    let time_without = |disabled: Option<&str>| -> f64 {
        let pcs_bldc_sil::Board { mut sim, .. } =
            board_with(Sil::options().grid_us(FINE_GRID_US), 0.8);
        sim.run_for_ms(GATE_BRINGUP_MS);
        if let Some(name) = disabled {
            assert!(sim.set_member_enabled(name, false), "member {name} exists");
        }
        time_avg_us(WARMUP, N, || {
            sim.step().expect("engine step");
        })
    };

    let full = time_without(None);
    let xrt = |us: f64| (FINE_GRID_US as f64) / us;
    println!("\n-- board-world report (informational) --");
    println!("         sim grid = {FINE_GRID_US} µs, bridge dark, avg over {N} steps");
    println!("         phase                              µs/step   ×realtime");
    println!(
        "         full board world                   {full:>7.2}   {:>6.1}×",
        xrt(full)
    );
    for (name, label) in [
        (MOTOR, "motor model         "),
        (SENSE, "current-sense model "),
        (ENCODER, "commutation encoder "),
        (DIAL, "dial encoder        "),
        (SOURCE, "firmware member     "),
    ] {
        let without = time_without(Some(name));
        println!(
            "           of which {label}     {:>7.2}   (full - world without it)",
            full - without
        );
    }
}

/// Build a fine-grid world of the same shape as the coarse full-step row (a model
/// driving a firmware input cvar through a route), optionally with a periodic interrupt
/// of `isr_period_us` on [`FINE_ISR`] and with `sweep_period_us` as its mirror cadence,
/// then time `n` steps after a warm-up.
fn time_fine_world(isr_period_us: Option<u64>, sweep_period_us: u64, warmup: u64, n: u64) -> f64 {
    const STEP: u32 = 25; // stays within the u8 destination byte
    let mut sim = Sil::options()
        .grid_us(FINE_GRID_US)
        .sweep_period_us(sweep_period_us)
        .build();
    sim.add_member(CountsRampModel::new("sensor", STEP));
    let mut fwm = sim.load_firmware(SOURCE);
    fwm.register_cvar_in_state_table("HW_USB_sim_data.rx[0]");
    if let Some(period_us) = isr_period_us {
        fwm.register_periodic_isr(FINE_ISR, period_us, 0)
            .expect("the handler name resolves to a function in the image");
    }
    sim.add_member(fwm);
    sim.add_route(
        vsig_id("sensor", "counts").expect("valid vsig id"),
        SignalId::new("cvar", SOURCE, "HW_USB_sim_data.rx[0]", None).expect("valid cvar id"),
    )
    .expect("add route");
    time_avg_us(warmup, n, || {
        sim.step().expect("engine step");
    })
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
