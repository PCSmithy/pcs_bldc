//! pcs_bldc SIL sanity suite (and demo).
//!
//! Loads this board's firmware shared library and proves that the REAL firmware
//! runs on the native scheduler: all four FreeRTOS tasks advance, the State Table
//! historian works, and one end-to-end path (AS5048 model -> duplex SPI ->
//! IO_AS5048 -> telemetryTask -> IO_serial -> HW_USB(sim capture)) carries a
//! model-commanded encoder angle out as Teleplot text.
//!
//! Sim-exercising checks drive the firmware through the [`voyant::Engine`] step loop
//! (advance time, advance models, propagate routes, run firmware, sample cvars); only
//! the backend-lifecycle checks (boot, shutdown) stay below the engine. Each check
//! prints PASS/FAIL and the process exits nonzero on any failure so
//! `tools/run_sil.sh` catches regressions. It also exercises the model + Route Table
//! seams on the real production path (model `vsig` → route → firmware `cvar` flush,
//! with suspend/resume) and the **port seam** end to end (sim ADC input ports carry a
//! model's volts, converted to counts by the driver). Logged warnings/errors are
//! drained and printed at the end.
//!
//! Usage: `cargo run -p pcs_bldc_sil -- [path-to-firmware-shared-lib]`
//!
//! Env vars:
//! - `PCS_SIL_DLL_FLAVOR` — label printed in the performance report (set by
//!   `tools/run_sil.sh`).
//! - `PCS_SIL_DIAG=1` — after boot, print a white-box per-tick scheduling table
//!   (FreeRTOS `xTickCount`, `xNextTaskUnblockTime`, and the per-task heartbeat
//!   counters, all read by DWARF straight from firmware memory). Print-only —
//!   it advances the shared firmware handle like any other check, so the delta
//!   bands the checks assert are unaffected. Exists to turn a CI runner into a
//!   remote debugger for the macOS aarch64 multi-tick cadence anomaly.

mod as5048;

use as5048::As5048Model;
use std::path::PathBuf;
use std::process::ExitCode;
use std::time::Instant;
use voyant::{
    vsig_id, DuplexHandle, DuplexPeer, Engine, EngineError, Firmware, FirmwareMember, LogEntry,
    LogLevel, Member, MemberCtx, RampModel, RouteError, SignalId, StateTable, Value,
};

const SOURCE: &str = "pcs_bldc";
const TICK_US: u64 = 1_000; // the firmware's 1 ms task cadence

fn default_lib_path() -> PathBuf {
    // The host shared-library flavour: .dll (Windows), .dylib (macOS), .so (Linux).
    let name = if cfg!(target_os = "windows") {
        "libpcs_bldc_fw.dll"
    } else if cfg!(target_os = "macos") {
        "libpcs_bldc_fw.dylib"
    } else {
        "libpcs_bldc_fw.so"
    };
    PathBuf::from("../../build/native-fw/src").join(name)
}

/// A `cvar:pcs_bldc:<path>` id for this board's firmware statics.
fn cvar(path: &str) -> SignalId {
    SignalId::new("cvar", SOURCE, path, None).expect("valid cvar id")
}

/// The `cvar:pcs_bldc:<path>` id **string** — for the string-keyed engine API
/// (`eng.write`/`eng.read`), which parses the id itself.
fn cid(path: &str) -> String {
    format!("cvar:{SOURCE}:{path}")
}

/// Running tally of the sanity suite; prints each check and counts failures, and
/// collects any Warning/Error log entries drained from the checks' State Tables.
struct Report {
    failures: u32,
    logs: Vec<LogEntry>,
}

impl Report {
    fn new() -> Self {
        Report {
            failures: 0,
            logs: Vec::new(),
        }
    }

    fn check(&mut self, name: &str, pass: bool, detail: String) {
        if pass {
            println!("  [PASS] {name}\n         {detail}");
        } else {
            self.failures += 1;
            println!("  [FAIL] {name}\n         {detail}");
        }
    }

    /// Absorb log entries drained from a check's engine, keeping Warning/Error.
    fn absorb(&mut self, entries: Vec<LogEntry>) {
        self.logs.extend(
            entries
                .into_iter()
                .filter(|e| matches!(e.level, LogLevel::Warning | LogLevel::Error)),
        );
    }
}

fn main() -> ExitCode {
    let path = std::env::args()
        .nth(1)
        .map(PathBuf::from)
        .unwrap_or_else(default_lib_path);

    println!("=== pcs_bldc SIL sanity suite ===");
    println!("loading firmware: {}", path.display());
    let fw = match Firmware::load(&path) {
        Ok(f) => f,
        Err(e) => {
            eprintln!("FATAL: failed to load firmware library: {e}");
            return ExitCode::FAILURE;
        }
    };

    let mut rep = Report::new();

    // --- Check 1: boot ------------------------------------------------------
    println!("\n-- 1. boot --");
    let booted = fw.start();
    rep.check("boot", booted, format!("sil_fw_start() -> {booted}"));
    if !booted {
        eprintln!("FATAL: cannot continue without a booted firmware");
        return ExitCode::FAILURE;
    }

    // --- Optional white-box diagnostic (PCS_SIL_DIAG=1) ---------------------
    // Print-only; advances the shared firmware handle like any check would.
    diag_per_tick_table(&fw);

    // --- Check 2: all four real tasks advance -------------------------------
    println!("\n-- 2. all four real FreeRTOS tasks advance --");
    check_tasks_advance(&fw, &mut rep);

    // --- Check 3: State Table historian / enum / ZOH ------------------------
    println!("\n-- 3. State Table (historian + enum + ZOH) --");
    check_state_table(&fw, &mut rep);

    // --- Check 4: end-to-end data path (flagship) ---------------------------
    println!("\n-- 4. end-to-end: encoder -> SPI(sim) -> telemetry -> USB(sim) --");
    check_end_to_end(&fw, &mut rep);

    // --- Check 5: model-backed (vsig) signal --------------------------------
    println!("\n-- 5. model-backed vsig signal (model Member) --");
    check_model_vsig(&fw, &mut rep);

    // --- Check 6: Route Table drives a firmware cvar from a model -----------
    println!("\n-- 6. route table (model vsig -> firmware cvar) --");
    check_route_table(&fw, &mut rep);

    // --- Check 7: two-member feedback loop (latency / validation) -----------
    println!("\n-- 7. feedback loop (model <-> firmware, delayed ZOH cut) --");
    check_feedback_loop(&fw, &mut rep);

    // --- Check 8: port seam (native-format volts -> ADC counts) -------------
    println!("\n-- 8. ports: model volts -> sim ADC port -> counts --");
    check_adc_ports(&fw, &mut rep);

    // --- Check 9: whole-namespace cvar mirror accuracy ----------------------
    println!("\n-- 9. cvar mirror accuracy (auto-derived, no declaration) --");
    check_mirror_accuracy(&fw, &mut rep);

    // --- Check 10: model <-> model duplex (no firmware) ---------------------
    println!("\n-- 10. model<->model duplex over spi (no firmware) --");
    check_model_duplex(&mut rep);

    // --- Check 11: AS5048 encoder model ----------------------------------
    println!("\n-- 11. AS5048 encoder model --");
    check_as5048_model(&mut rep);

    // --- Check 12: PWM/bridge observation ports -----------------------------
    println!("\n-- 12. PWM/bridge observation ports --");
    check_pwm_ports(&fw, &mut rep);

    // --- Performance report (phase-isolated, informational) -----------------
    println!("\n-- performance report (phase-isolated, informational) --");
    report_performance(&fw);

    // --- Check 13: shutdown -------------------------------------------------
    println!("\n-- 13. shutdown --");
    fw.shutdown();
    rep.check("shutdown", true, "sil_fw_shutdown() returned cleanly".into());

    // Informational: surface any Warning/Error entries the checks logged into
    // their State Tables (does not affect pass/fail for now).
    if rep.logs.is_empty() {
        println!("\n-- no warnings/errors logged during the run --");
    } else {
        println!("\n-- {} warning/error log entrie(s) --", rep.logs.len());
        for e in &rep.logs {
            println!("  {e}");
        }
    }

    println!("\n=== {} check(s) failed ===", rep.failures);
    if rep.failures == 0 {
        println!("all checks PASS");
        ExitCode::SUCCESS
    } else {
        ExitCode::FAILURE
    }
}

/// White-box per-tick scheduling table, gated by `PCS_SIL_DIAG=1` (no-op
/// otherwise). Straight after boot, advance the firmware one raw tick at a time
/// and, each tick, DWARF-read the FreeRTOS kernel's own view — `xTickCount` and
/// `xNextTaskUnblockTime` (the earliest tick any blocked task is scheduled to
/// wake) — alongside the per-task heartbeat counters. A general-purpose remote
/// debugger for scheduling / DWARF-resolution questions:
/// - if `xTickCount` advances by 1/tick but `xNextTaskUnblockTime` sits at
///   `xTickCount+1` every tick (and every counter climbs each tick), the block
///   times themselves are wrong (kernel/port bug — the delayed-list wake values
///   are being computed or stored as ~1 tick);
/// - if the counters climb but `xNextTaskUnblockTime` looks sane (e.g. +10 for
///   the 10 ms task), the anomaly is in how the suite reads the counters, not in
///   the firmware's scheduling (DWARF/address resolution).
///
/// The resolved-address list also surfaces per-variable DWARF faults directly:
/// two distinct statics collapsing to one address means their reads alias.
///
/// Print-only: it advances the shared `fw` handle exactly as the checks do
/// (each check reads its own `before`/`after` deltas), so it does not perturb
/// any pass/fail band. Reads are panic-guarded so a firmware built without a
/// given static degrades that column to `n/a` rather than aborting the suite.
fn diag_per_tick_table(fw: &Firmware) {
    if std::env::var("PCS_SIL_DIAG").ok().as_deref() != Some("1") {
        return;
    }
    const TICKS: u32 = 15;

    // Silence the default panic hook for the duration: a missing DWARF symbol
    // makes `read_cvar` panic, which we catch below and render as "n/a" — we do
    // not want the backtrace spew for an expected, handled miss.
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
    println!("         DLL flavor: {}", std::env::var("PCS_SIL_DLL_FLAVOR").unwrap_or_else(|_| "unknown".into()));

    // Resolved runtime addresses of the statics the table reads. If two distinct
    // statics collapse to one address here, their reads alias — a per-variable
    // DWARF-resolution fault (e.g. same-TU file-scope `static`s on Mach-O),
    // NOT a firmware/scheduling bug.
    println!("         resolved addresses (watch for collisions):");
    for p in [
        "xTickCount", "task1msRuns", "task10msRuns", "telemRuns",
        "task200msRuns", "taskUsbRuns",
    ] {
        let a = fw.resolve_addr(p).map(|a| format!("{a:#018x}")).unwrap_or_else(|| "n/a".into());
        println!("           {p:<16} {a}");
    }

    println!("         {:>4}  {:>10}  {:>9}  {:>7}  {:>8}  {:>5}  {:>7}",
             "tick", "xTickCount", "nextUnblk", "task1ms", "task10ms", "telem", "taskUsb");
    // Row 0 = post-boot baseline (all tasks just blocked; no tick applied yet).
    // Rows 1.. = state after each raw firmware tick.
    for i in 0..=TICKS {
        println!("         {:>4}  {:>10}  {:>9}  {:>7}  {:>8}  {:>5}  {:>7}",
                 i,
                 rd("xTickCount"),
                 rd("xNextTaskUnblockTime"),
                 rd("task1msRuns"),
                 rd("task10msRuns"),
                 rd("telemRuns"),
                 rd("taskUsbRuns"));
        if i < TICKS {
            fw.advance_tick();
        }
    }

    std::panic::set_hook(prev_hook);
}

/// Read the four per-task heartbeat counters, advance ~50 ticks **through the
/// engine**, and assert each task advanced by roughly its expected count for its
/// period. The engine samples the counters into its historian each tick, so the
/// post-window values are read straight from the State Table.
fn check_tasks_advance(fw: &Firmware, rep: &mut Report) {
    const COUNTERS: [&str; 4] = ["task1msRuns", "task10msRuns", "taskUsbRuns", "telemRuns"];
    const N: u64 = 50;

    // Direct read: the cvar mirror is cold until the first step, so read the baseline
    // counters from firmware memory.
    let before: Vec<u64> = COUNTERS
        .iter()
        .map(|c| fw.read_cvar(c).as_u64().unwrap_or(0))
        .collect();

    let mut eng = Engine::new(TICK_US);
    let ids: Vec<SignalId> = COUNTERS.iter().map(|c| cvar(c)).collect();
    // No per-signal declaration: the FirmwareMember auto-mirrors the whole cvar
    // namespace, so these counters are sampled into the historian each tick.
    eng.add_member(FirmwareMember::new(SOURCE, fw, TICK_US));
    for _ in 0..N {
        eng.step().expect("engine step");
    }
    // Post-window counter values come from the engine's own historian (auto-mirrored).
    let after: Vec<u64> = ids
        .iter()
        .map(|id| {
            eng.read(id.as_str())
                .ok()
                .flatten()
                .as_ref()
                .and_then(Value::as_u64)
                .unwrap_or(0)
        })
        .collect();
    let d: Vec<u64> = before
        .iter()
        .zip(&after)
        .map(|(b, a)| a.saturating_sub(*b))
        .collect();

    // 1 ms task fires once per tick; 10 ms every 10; telemetry every 2 ms; USB
    // delays 1 tick per iteration (so ~once per tick). Tolerance bands, not
    // exact equality (scheduling phase can shift a fire in or out of the window).
    rep.check(
        "task_1ms advances (~50 / 50 ticks)",
        (45..=55).contains(&d[0]),
        format!("task1msRuns +{}", d[0]),
    );
    rep.check(
        "task_10ms advances (~5 / 50 ticks)",
        (3..=7).contains(&d[1]),
        format!("task10msRuns +{}", d[1]),
    );
    rep.check(
        "task_usb advances (~50 / 50 ticks)",
        (40..=60).contains(&d[2]),
        format!("taskUsbRuns +{}", d[2]),
    );
    rep.check(
        "telemetryTask advances (~25 / 50 ticks @ 2 ms)",
        (20..=30).contains(&d[3]),
        format!("telemRuns +{}", d[3]),
    );
    rep.absorb(eng.take_logs());
}

/// Drive the engine for a few ticks with a ramping ADC signal registered as a
/// sampled cvar; the engine records it into its historian each tick. Assert the
/// change-log accumulates and a ZOH historical lookup holds the prior sample;
/// read an enum cvar symbolically.
fn check_state_table(fw: &Firmware, rep: &mut Report) {
    let ramp = cvar("HW_ADC_data.channelData[0].counts[6]");
    let mut eng = Engine::new(TICK_US);
    // Auto-mirrored: counts[19] is under the array threshold, so counts[6] is
    // registered + sampled with no `sample_cvar` declaration.
    eng.add_member(FirmwareMember::new(SOURCE, fw, TICK_US));

    let mut samples = Vec::new();
    for _ in 1..=6u64 {
        eng.step().expect("engine step");
        let v = eng.read(ramp.as_str()).ok().flatten();
        samples.push(v.as_ref().and_then(Value::as_u64).unwrap_or(0));
    }

    let changed = samples.windows(2).any(|w| w[0] != w[1]);
    let n_changes = eng.state().changes(&ramp).unwrap().len();
    rep.check(
        "historian records a changing ADC ramp signal",
        changed && (n_changes >= 2),
        format!("counts[6] samples {samples:?}, {n_changes} change-log entries"),
    );

    // Zero-order-hold: a lookup between samples holds the prior value.
    let mid = (2 * TICK_US) + 500;
    let zoh = eng.state().value_at(&ramp, mid).unwrap();
    rep.check(
        "ZOH historical lookup holds the prior sample",
        zoh.is_some(),
        format!("value_at({mid}us) = {zoh:?}"),
    );

    // Enum-typed cvar resolves to its symbolic DWARF enumerator name (auto-mirrored;
    // the boxed lane carries the Enum value after stepping).
    let tm = eng
        .read(&cid("HW_ADC_channelConfig[0].triggerMode"))
        .ok()
        .flatten()
        .unwrap_or(Value::U32(0));
    rep.check(
        "enum cvar reads as a symbolic name",
        matches!(tm, Value::Enum(_)),
        format!("HW_ADC_channelConfig[0].triggerMode = {tm}"),
    );
    rep.absorb(eng.take_logs());
}

/// The flagship check: command the AS5048 encoder model's angle through the unit
/// boundary and read it back out of real firmware telemetry — model answers the
/// firmware's actual READ-ANGLE polls over the duplex bus, IO_AS5048 decodes,
/// telemetryTask reports, the sim USB capture carries the Teleplot text.
fn check_end_to_end(fw: &Firmware, rep: &mut Report) {
    const CH: usize = 0; // HW_SPI_CHANNEL_AS5048_1
    const CMD_DEG: f64 = 90.0;

    // Both encoder channels get a real model instance. Member order [models,
    // firmware] gives zero-lag freshness: a model folds its commanded angle before
    // the firmware's same-tick SPI polls read it. (Tick 1's first poll returns the
    // model's power-on sentinel — an error frame — exactly like real hardware's
    // first pipelined read; the second poll onward carries the angle.)
    let mut eng = Engine::new(TICK_US);
    let motor = eng.add_member(As5048Model::new("as5048_motor", 0.0));
    let dial = eng.add_member(As5048Model::new("dial", 0.0));
    eng.add_member(FirmwareMember::new(SOURCE, fw, TICK_US));
    eng.link_duplex("spi:pcs_bldc:AS5048_1", motor).expect("link motor encoder");
    eng.link_duplex("spi:pcs_bldc:AS5048_2", dial).expect("link dial encoder");

    // Both encoder channels register their SPI endpoints as :tx/:rx event entries —
    // the dial (AS5048_2) too, which stage 3 drives as the velocity demand.
    let ep = |ch: &str, m: &str| format!("spi:{SOURCE}:{ch}:{m}");
    let endpoints_present = ["AS5048_1", "AS5048_2"].iter().all(|ch| {
        ["tx", "rx"]
            .iter()
            .all(|m| eng.state().signals().any(|s| s.as_str() == ep(ch, m)))
    });
    rep.check(
        "both AS5048 duplex endpoints registered in the State Table",
        endpoints_present,
        format!(
            "{}, {} present={endpoints_present}",
            ep("AS5048_1", "rx"),
            ep("AS5048_2", "rx")
        ),
    );

    // Pre-step config read (static value; the mirror is cold until the first step).
    // Expected raw/angle derive from the commanded angle plus the firmware's own
    // channel config (reverse maps raw to 16384 - raw): the check asserts the
    // decode contract, not a frozen board convention.
    let reverse = match fw.read_cvar(&format!("IO_AS5048_channelConfig[{CH}].reverse")) {
        Value::Bool(b) => b,
        other => panic!("IO_AS5048_channelConfig[{CH}].reverse read back as {other:?}, not Bool"),
    };
    let wire_raw = ((CMD_DEG / 360.0) * 16384.0).round() as u64; // the model's frame
    let exp_raw: u64 = if reverse { 16384 - wire_raw } else { wire_raw };
    let exp_deg = (exp_raw as f64) * 360.0 / 16384.0;
    let exp_deg_str = format!("{exp_deg:.2}");

    // Command the shaft angle in degrees — the boundary converts to canonical rad.
    // Open the USB port (telemetryTask skips its body otherwise) and drain the
    // stale TX capture; the cvar flush lands during step 1's in-sync.
    eng.write("vsig:as5048_motor:angle[deg]", CMD_DEG).expect("write angle[deg]");
    eng.write(&cid("HW_USB_sim_data.connected"), true).expect("write connected");
    eng.write(&cid("HW_USB_sim_data.txLen"), 0u32).expect("drain txLen");

    // (3) Advance: task_1ms samples the encoder each tick; telemetry fires every 2 ms.
    for _ in 0..10 {
        eng.step().expect("engine step");
    }

    // (3z) The SPI transactions land as event records: the AS5048 does two transfers
    // per tick, and events are force-recorded (never deduped), so :tx / :rx each
    // accumulate more entries than the 10 ticks would allow under level dedup. :tx
    // carries the READ-ANGLE command (0xFFFF -> {0xFF,0xFF}); :rx the model's angle
    // frame (raw 4096 + parity = 0x9000 -> {0x90,0x00}).
    let tx_id = SignalId::parse(&ep("AS5048_1", "tx")).expect("valid spi id");
    let rx_id = SignalId::parse(&ep("AS5048_1", "rx")).expect("valid spi id");
    let n_tx = eng.state().changes(&tx_id).map(|c| c.len()).unwrap_or(0);
    let n_rx = eng.state().changes(&rx_id).map(|c| c.len()).unwrap_or(0);
    let last_tx = eng.state().current_value(&tx_id).ok().flatten();
    let last_rx = eng.state().current_value(&rx_id).ok().flatten();
    rep.check(
        "SPI transactions recorded as :tx/:rx events (2 transfers/tick, not deduped)",
        (n_tx > 10) && (n_rx > 10)
            && (last_tx == Some(Value::Bytes(vec![0xFF, 0xFF])))
            && (last_rx == Some(Value::Bytes(vec![0x90, 0x00]))),
        format!("{n_tx} tx + {n_rx} rx events over 10 ticks; last tx={last_tx:?}, rx={last_rx:?}"),
    );

    // (3a) The connected flag reached firmware (the mirror reads it back true).
    let connected = matches!(
        eng.read(&cid("HW_USB_sim_data.connected")).ok().flatten(),
        Some(Value::Bool(true))
    );
    rep.check(
        "sim USB marked connected via table write + flush",
        connected,
        "HW_USB_sim_data.connected mirrors true after the flush".into(),
    );

    // (3b) The decoded encoder statics reflect the model's frame (auto-mirrored).
    let raw = eng
        .read(&cid(&format!("IO_AS5048_data.channels[{CH}].raw")))
        .ok()
        .flatten()
        .as_ref()
        .and_then(Value::as_u64)
        .unwrap_or(0);
    let deg = eng
        .read(&cid(&format!("IO_AS5048_data.channels[{CH}].angle_deg")))
        .ok()
        .flatten()
        .as_ref()
        .and_then(Value::as_f64)
        .unwrap_or(0.0);
    rep.check(
        "AS5048 decodes the model's SPI frame",
        (raw == exp_raw) && ((deg - exp_deg).abs() < 0.05),
        format!("IO_AS5048_data.channels[0].raw = {raw} (expect {exp_raw}, reverse={reverse}), angle_deg = {deg:.2} (expect {exp_deg_str})"),
    );

    // (3c) Pull the sim USB TX capture and confirm the Teleplot telemetry text.
    //      Direct read: tx[] is over the mirror threshold — see backlog usb_cdc/teleplot.
    let text = read_tx_capture(fw);
    if let Some(first) = text.lines().next() {
        println!("         telemetry[0]: {first}");
    }
    let has_keys = text.contains("motor_angle:") && text.contains("motor_raw:");
    let has_angle = text.contains(&exp_deg_str);
    let has_raw = text.contains(&exp_raw.to_string());
    rep.check(
        "telemetry text present with expected signal keys",
        has_keys,
        format!("captured {} bytes; motor_angle/motor_raw keys {}", text.len(),
                if has_keys { "present" } else { "MISSING" }),
    );
    rep.check(
        "telemetry carries the commanded angle end-to-end (exact)",
        has_angle && has_raw,
        format!("text contains angle {exp_deg_str} = {has_angle}, raw {exp_raw} = {has_raw}"),
    );

    // (4) Drain the capture (txLen back to 0, flushed on the next step's in-sync
    //     before telemetry runs) and re-verify the next windows refill it — proves the
    //     path keeps flowing and the drain works.
    eng.write(&cid("HW_USB_sim_data.txLen"), 0u32).expect("drain txLen");
    for _ in 0..6 {
        eng.step().expect("engine step");
    }
    let text2 = read_tx_capture(fw);
    // motor_angle goes out in the fast (2 ms) tier every window; motor_raw is
    // slow-tier (200 ms) and won't appear in a short post-drain capture.
    rep.check(
        "TX capture drains and refills across windows",
        text2.contains(&exp_deg_str) && text2.contains("motor_angle:"),
        format!("post-drain capture {} bytes, still carries the angle", text2.len()),
    );
    rep.absorb(eng.take_logs());
}

/// Demonstrate the `vsig` backing driven by the engine: a reference [`RampModel`]
/// is registered as a model, and the engine advances it with sim time and records
/// it through the same historian machinery as cvar samples each [`Engine::step`].
/// The firmware ticks alongside but is irrelevant to the model's own `vsig`.
fn check_model_vsig(fw: &Firmware, rep: &mut Report) {
    let mut eng = Engine::new(TICK_US);
    eng.add_member(RampModel::new("demo", 1000.0, Some("counts"))); // +1.0 / ms
    // The firmware ticks alongside (irrelevant to the model's own vsig).
    eng.add_member(FirmwareMember::new(SOURCE, fw, TICK_US));
    let id = vsig_id("demo", "value").expect("valid vsig id");
    // Registered by the model at add, but not yet recorded (read -> Ok(None)).
    let registered = eng.read(id.as_str()).map(|v| v.is_none()).unwrap_or(false);
    rep.check(
        "model registers a vsig signal into the State Table",
        registered,
        format!("registered {} ({} signal(s) in table)", id, eng.state().len()),
    );

    for _ in 1..=5u64 {
        eng.step().expect("engine step");
    }
    let n_changes = eng.state().changes(&id).map(|c| c.len()).unwrap_or(0);
    let last = eng.read(id.as_str()).ok().flatten();
    rep.check(
        "vsig advances with sim time and the historian records it",
        (n_changes == 5) && matches!(&last, Some(Value::F64(v)) if (*v - 5.0).abs() < 1e-9),
        format!("{n_changes} change-log entries, current = {last:?} (expect F64(5.0))"),
    );
    rep.absorb(eng.take_logs());
}

/// A minimal integer "sensor" model for the route demo: one `counts` signal stepping
/// a fixed amount each tick. Emits [`Value::U32`] so a route drives a firmware
/// `uint32_t` with no conversion (routes are pure copies; float→counts is a sensor
/// model's job). Board-specific models live on the instantiation side.
struct CountsRampModel {
    name: String,
    step: u32,
    counts: u32,
}

impl CountsRampModel {
    fn new(name: &str, step: u32) -> Self {
        Self {
            name: name.to_string(),
            step,
            counts: 0,
        }
    }

    fn counts_id(&self) -> SignalId {
        vsig_id(&self.name, "counts").expect("valid vsig id")
    }
}

impl Member for CountsRampModel {
    fn name(&self) -> &str {
        &self.name
    }
    fn advance(&mut self, _dt_us: u64, ctx: &mut MemberCtx) {
        self.counts = self.counts.wrapping_add(self.step);
        let id = self.counts_id();
        if let Err(e) = ctx.st.record(&id, Value::U32(self.counts)) {
            ctx.st.log(LogLevel::Warning, &self.name, format!("record {id} failed: {e}"));
        }
    }
    fn set_enabled(&mut self, on: bool, st: &mut StateTable) {
        if on {
            let _ = st.register(self.counts_id(), Some("counts"));
        }
    }
}

/// Prove the Route Table end-to-end on the engine via the real production path: a
/// model's `vsig` is routed table→table into a firmware `cvar` entry, and the
/// [`FirmwareMember`] flushes it into memory before `advance_tick` (plant output →
/// firmware sensor input). Suspend/resume gates the route.
///
/// The destination is the sim USB rx buffer's byte 0 — a firmware input the firmware
/// *reads* but never writes (only the sim's injectRx writes `rx[]`), so a flushed
/// value survives the tick and can be asserted after. Member order `[model, firmware]`
/// gives zero-lag tracking; the model steps by 25 to stay within the byte.
fn check_route_table(fw: &Firmware, rep: &mut Report) {
    const STEP: u32 = 25; // stays within a u8 destination byte
    let src = vsig_id("sensor", "counts").expect("valid vsig id");
    // The sim USB rx byte: read by the firmware, never written by it.
    let dst = SignalId::new("cvar", SOURCE, "HW_USB_sim_data.rx[0]", None).expect("valid cvar id");

    let mut eng = Engine::new(TICK_US);
    eng.add_member(CountsRampModel::new("sensor", STEP));
    let mut fwm = FirmwareMember::new(SOURCE, fw, TICK_US);
    // rx is a 512-byte buffer, over the array threshold, so the driven element is
    // registered explicitly. A registered cvar the framework command-writes (the
    // route) is flushed into memory by the member automatically (fresh-dirty flush).
    fwm.register_cvar_in_state_table("HW_USB_sim_data.rx[0]");
    eng.add_member(fwm);
    eng.add_route(src.clone(), dst.clone()).expect("add route");

    // Active route: firmware memory tracks the model exactly, step by step. Direct
    // reads: this check asserts the routed value reached firmware MEMORY (the member's
    // flush), not just the table.
    let mut tracked = true;
    let mut last = 0u64;
    for tick in 1..=4u64 {
        eng.step().expect("engine step");
        let got = fw.read_cvar(dst.name()).as_u64().unwrap_or(0);
        tracked &= got == (tick * STEP as u64);
        last = got;
    }
    rep.check(
        "route drives a firmware cvar from a model vsig (via FirmwareMember flush)",
        tracked && (last == 4 * STEP as u64),
        format!("rx[0] tracked the model to {last} over 4 steps (expect {})", 4 * STEP),
    );

    // Suspend: the model keeps advancing, but the route stops recording the dest
    // entry, so the firmware member re-flushes the held value — firmware memory
    // must NOT follow the model.
    eng.suspend_route(&src, &dst).expect("suspend");
    eng.step().expect("engine step");
    let held = fw.read_cvar(dst.name()).as_u64().unwrap_or(0); // direct: verifies fw memory
    let model_now = eng
        .read(src.as_str())
        .ok()
        .flatten()
        .as_ref()
        .and_then(Value::as_u64)
        .unwrap_or(0);
    rep.check(
        "suspended route stops driving its destination",
        (held == last) && (model_now > held),
        format!("rx[0] held at {held} while model advanced to {model_now}"),
    );

    // Resume: the destination jumps to the model's current value again.
    eng.resume_route(&src, &dst).expect("resume");
    eng.step().expect("engine step");
    let resumed = fw.read_cvar(dst.name()).as_u64().unwrap_or(0); // direct: verifies fw memory
    let model_after = eng
        .read(src.as_str())
        .ok()
        .flatten()
        .as_ref()
        .and_then(Value::as_u64)
        .unwrap_or(0);
    rep.check(
        "resumed route drives the destination again",
        resumed == model_after,
        format!("rx[0] = {resumed} after resume (expect current model {model_after})"),
    );
    rep.absorb(eng.take_logs());
}

/// A model member for the feedback loop: reads input `in` (a firmware counter,
/// delivered on the *delayed* backward edge) and emits `out = in % 200` (a
/// [`Value::U32`] within a `u8` so it can drive the sim USB `rx[0]` byte with no
/// conversion). The modulo keeps `out` in range regardless of the firmware
/// counter's absolute value.
struct LoopModel {
    name: String,
    out: u32,
}

impl LoopModel {
    fn new(name: &str) -> Self {
        Self {
            name: name.to_string(),
            out: 0,
        }
    }
    fn in_id(&self) -> SignalId {
        vsig_id(&self.name, "in").expect("valid vsig id")
    }
    fn out_id(&self) -> SignalId {
        vsig_id(&self.name, "out").expect("valid vsig id")
    }
}

impl Member for LoopModel {
    fn name(&self) -> &str {
        &self.name
    }
    fn advance(&mut self, _dt_us: u64, ctx: &mut MemberCtx) {
        let input = ctx
            .st
            .current_value(&self.in_id())
            .ok()
            .flatten()
            .as_ref()
            .and_then(Value::as_u64)
            .unwrap_or(0);
        self.out = (input % 200) as u32;
        let id = self.out_id();
        if let Err(e) = ctx.st.record(&id, Value::U32(self.out)) {
            ctx.st.log(LogLevel::Warning, &self.name, format!("record {id} failed: {e}"));
        }
    }
    fn set_enabled(&mut self, on: bool, st: &mut StateTable) {
        if on {
            let _ = st.register(self.in_id(), None);
            let _ = st.register(self.out_id(), Some("counts"));
        }
    }
}

/// Prove the per-route latency design as a genuine **two-member feedback loop**:
/// - forward (zero-latency): the model's `out` drives a firmware sim-input byte
///   (read but never written, so a flushed value survives the tick);
/// - backward (DELAYED, the ZOH cut): the firmware counter `task1msRuns` → model `in`.
///
/// With member order `[model, firmware]`, the backward edge is backward in
/// registration order: the validator rejects it while zero-latency, accepts it once
/// delayed. We catch the step error, rewire **live**, then assert the exact
/// deterministic sequence.
fn check_feedback_loop(fw: &Firmware, rep: &mut Report) {
    let out = vsig_id("loop_model", "out").expect("valid vsig id");
    let inp = vsig_id("loop_model", "in").expect("valid vsig id");
    let counter = cvar("task1msRuns"); // firmware output (sampled)
    let rx = cvar("HW_USB_sim_data.rx[0]"); // firmware input (driven)

    let mut eng = Engine::new(TICK_US);
    eng.add_member(LoopModel::new("loop_model")); // idx 0
    let mut fwm = FirmwareMember::new(SOURCE, fw, TICK_US); // idx 1
    // The rx byte lives in a 512-byte buffer (over threshold) -> register it
    // explicitly so the route can drive it. `task1msRuns` (the sampled counter) is a
    // top-level scalar, auto-mirrored with no declaration.
    fwm.register_cvar_in_state_table("HW_USB_sim_data.rx[0]");
    eng.add_member(fwm);

    // Forward edge: model out -> firmware rx byte (zero-latency, model before fw).
    eng.add_route(out.clone(), rx.clone()).expect("add forward route");
    // Backward edge as ZERO-latency first: firmware counter -> model in. This is a
    // backward edge (source firmware is registered AFTER the consuming model), so
    // the validator must reject it at the next step.
    eng.add_route(counter.clone(), inp.clone()).expect("add backward route");

    let rejected = matches!(
        eng.step(),
        Err(EngineError::Route(RouteError::BackwardRoute { .. }))
    );
    rep.check(
        "validator rejects the zero-latency feedback loop (backward edge)",
        rejected,
        "step() -> BackwardRoute until the backward edge is declared delayed".into(),
    );

    // Fix the wiring LIVE: drop the zero-latency backward edge, re-add it delayed
    // (the explicit ZOH sample/actuation cut). Rewire-at-runtime is legal.
    eng.remove_route(&counter, &inp).expect("remove backward route");
    eng.add_delayed_route(counter.clone(), inp.clone())
        .expect("add delayed backward route");

    // Predicted deterministic sequence for rx[0] read after each step:
    //   step 1: model in is unset (fw counter not yet sampled by this engine) -> 0.
    //   step k>=2: in = task1msRuns as of the end of step k-1 = base + (k-1),
    //             so rx[0] = (base + (k-1)) % 200.
    // `base` is the firmware counter right before the first successful step (the
    // rejected step returned early and did NOT advance the firmware). Direct reads:
    // the check verifies the routed value reached firmware MEMORY (the delayed feedback
    // edge flushing through the member).
    let base = fw.read_cvar(counter.name()).as_u64().unwrap_or(0);
    const N: u64 = 6;
    let got: Vec<u64> = (1..=N)
        .map(|_| {
            eng.step().expect("engine step");
            fw.read_cvar(rx.name()).as_u64().unwrap_or(0)
        })
        .collect();
    let predicted: Vec<u64> = (1..=N)
        .map(|k| if k == 1 { 0 } else { (base + (k - 1)) % 200 })
        .collect();

    rep.check(
        "delayed feedback loop produces the exact predicted sequence",
        got == predicted,
        format!("rx[0] over {N} steps = {got:?}, predicted {predicted:?} (base counter {base})"),
    );
    rep.absorb(eng.take_logs());
}

/// A model member holding one output `volts` at a constant voltage — the
/// simplest stand-in for a plant/sensor model driving an analog pin.
struct VoltsModel {
    name: String,
    volts: f64,
}

impl VoltsModel {
    fn new(name: &str, volts: f64) -> Self {
        Self {
            name: name.to_string(),
            volts,
        }
    }
    fn volts_id(&self) -> SignalId {
        vsig_id(&self.name, "volts").expect("valid vsig id")
    }
}

impl Member for VoltsModel {
    fn name(&self) -> &str {
        &self.name
    }
    fn advance(&mut self, _dt_us: u64, ctx: &mut MemberCtx) {
        let id = self.volts_id();
        if let Err(e) = ctx.st.record(&id, Value::F64(self.volts)) {
            ctx.st.log(LogLevel::Warning, &self.name, format!("record {id} failed: {e}"));
        }
    }
    fn set_enabled(&mut self, on: bool, st: &mut StateTable) {
        if on {
            let _ = st.register(self.volts_id(), Some("V"));
        }
    }
}

/// Prove the **port seam** end to end on the native-format path: the sim ADC driver
/// registered one input port per enabled input (during `sil_fw_start`). A model emits
/// a *voltage*; a zero-latency route carries it (native volts, member to member) into
/// the firmware's port entry `vsig:pcs_bldc:ADC1_IN6`; the [`FirmwareMember`] caches it
/// for C; and the driver converts volts -> counts with its own numBits/vref. Assert the
/// exact quantization, and that a NEIGHBORING undriven input keeps its ramp (the fallback).
fn check_adc_ports(fw: &Firmware, rep: &mut Report) {
    const VOLTS: f64 = 1.234;
    // ADC1 regular input 6 (port ADC1_IN6) is driven; input 1 stays undriven.
    let port = SignalId::new("vsig", SOURCE, "ADC1_IN6", None).expect("valid port id");
    let driven_counts = "HW_ADC_data.channelData[0].counts[6]";
    let neighbor_counts = "HW_ADC_data.channelData[0].counts[1]";

    let mut eng = Engine::new(TICK_US);
    eng.add_member(VoltsModel::new("pin_model", VOLTS));
    eng.add_member(FirmwareMember::new(SOURCE, fw, TICK_US));

    // The sim drivers register their ports during sil_fw_start; the FirmwareMember
    // applied them to the table at add_member. Filter to the ADC-named input ports:
    // the board config enables 10 regular inputs (5 per ADC), so 10 exist under this
    // member's name (the TIM PWM/bridge ports register under their own names).
    let n_ports = eng
        .state()
        .signals()
        .filter(|s| (s.sig_type() == "vsig") && (s.source() == SOURCE) && s.name().starts_with("ADC"))
        .count();
    rep.check(
        "sim ADC registered one input port per enabled input",
        n_ports == 10,
        format!("{n_ports} vsig:{SOURCE}:ADC* port(s) in the table (expect 10)"),
    );

    // Route the model's volts into the port (native format: volts -> volts).
    eng.add_route(vsig_id("pin_model", "volts").expect("valid vsig id"), port.clone())
        .expect("add route");

    // Expected counts: mirror of the sim driver's volts->counts math
    // (HW_ADC_private_voltsToCounts), with numBits/vref read via DWARF from the sim
    // channel config rather than hardcoded. Direct reads: static config, read before
    // the step loop while the mirror is still cold.
    let vref = fw.read_cvar("HW_ADC_channelConfig[0].vref").as_f64().unwrap_or(0.0);
    let num_bits = fw.read_cvar("HW_ADC_channelConfig[0].numBits").as_u64().unwrap_or(0);
    let max_counts = (1u64 << num_bits) - 1;
    let scaled = ((VOLTS / vref) * (max_counts as f64)) + 0.5;
    let expected = if scaled >= (max_counts as f64) {
        max_counts
    } else {
        scaled as u64
    };

    // Step a few ticks; sample both counts statics after each. The driven
    // input must sit at the exact quantized value once the port takes effect;
    // the undriven neighbor must keep ramping (distinct values across the
    // window — task_1ms fires at least once per engine step after the first).
    let mut driven: Vec<u64> = Vec::new();
    let mut neighbor: Vec<u64> = Vec::new();
    for _ in 0..6 {
        eng.step().expect("engine step");
        driven.push(eng.read(&cid(driven_counts)).ok().flatten().as_ref().and_then(Value::as_u64).unwrap_or(0));
        neighbor.push(eng.read(&cid(neighbor_counts)).ok().flatten().as_ref().and_then(Value::as_u64).unwrap_or(0));
    }
    let settled = &driven[2..];
    rep.check(
        "driven port converts volts -> counts via the sim's numBits/vref",
        settled.iter().all(|c| *c == expected),
        format!(
            "counts[6] = {settled:?} (expect {expected} = {VOLTS} V @ {num_bits} bits / {vref} V vref)"
        ),
    );
    let neighbor_settled = &neighbor[2..];
    let ramping = neighbor_settled.windows(2).any(|w| w[0] != w[1]);
    rep.check(
        "neighboring undriven input still ramps (fallback intact)",
        ramping,
        format!("counts[1] = {neighbor_settled:?} (must keep changing)"),
    );
    rep.absorb(eng.take_logs());
}

/// Prove the whole-namespace cvar mirror is **accurate and automatic**: pick a
/// firmware static that no other check declares or samples
/// (`HW_ADC_data.tickCounter`, the sim ADC's free-running counter), step the
/// engine with a plain [`FirmwareMember`] (zero declarations), and assert the
/// State Table entry both *tracks* (changes across the window) and *equals* a
/// fresh DWARF read of the same static — with no `sample_cvar` anywhere.
fn check_mirror_accuracy(fw: &Firmware, rep: &mut Report) {
    let leaf = cvar("HW_ADC_data.tickCounter");
    let mut eng = Engine::new(TICK_US);
    eng.add_member(FirmwareMember::new(SOURCE, fw, TICK_US));

    let mut vals: Vec<Option<u64>> = Vec::new();
    for _ in 0..6 {
        eng.step().expect("engine step");
        vals.push(eng.read(leaf.as_str()).ok().flatten().as_ref().and_then(Value::as_u64));
    }
    // The mirror at the end of the last tick equals firmware memory now (single-
    // threaded: nothing changed memory between the sweep and this read). `mem_now` is a
    // direct DWARF read: the ground truth the mirror (via eng.read) is checked against.
    let table_now = eng.read(leaf.as_str()).ok().flatten().as_ref().and_then(Value::as_u64);
    let mem_now = fw.read_cvar(leaf.name()).as_u64();
    let tracks = table_now.is_some() && (table_now == mem_now);
    let changed = vals.windows(2).any(|w| w[0] != w[1]);
    rep.check(
        "cvar mirror tracks firmware memory (auto-derived, no declaration)",
        tracks && changed,
        format!("table {table_now:?} == memory {mem_now:?}; window {vals:?}"),
    );
    rep.absorb(eng.take_logs());
}

/// A duplex responder model — one struct, both roles: as a [`DuplexPeer`] it answers
/// each SPI transfer with its current 14-bit angle framed big-endian; as a [`Member`]
/// it advances that angle every tick and records it as a `vsig`. Its state is its own —
/// a stand-in AS5048 with no firmware anywhere in the loop.
struct DialResponder {
    name: String,
    angle: u16,
    step: u16,
}

impl DuplexPeer for DialResponder {
    fn transfer(&mut self, _tx: &[u8]) -> Vec<u8> {
        (self.angle & 0x3FFF).to_be_bytes().to_vec()
    }
}

impl Member for DialResponder {
    fn name(&self) -> &str {
        &self.name
    }
    fn advance(&mut self, _dt_us: u64, ctx: &mut MemberCtx) {
        self.angle = self.angle.wrapping_add(self.step);
        let id = vsig_id(&self.name, "angle").expect("valid vsig id");
        let _ = ctx.st.record(&id, Value::U32(u32::from(self.angle)));
    }
    fn set_enabled(&mut self, on: bool, st: &mut StateTable) {
        if on {
            let _ = st.register(vsig_id(&self.name, "angle").expect("valid vsig id"), None);
        }
    }
}

/// A duplex initiator model: initiates a READ-ANGLE transfer each advance over its own
/// endpoint and records the decoded angle as a `vsig`.
struct DialInitiator {
    name: String,
    handle: DuplexHandle,
}

impl Member for DialInitiator {
    fn name(&self) -> &str {
        &self.name
    }
    fn advance(&mut self, _dt_us: u64, ctx: &mut MemberCtx) {
        if let Some(rx) = ctx.duplex_transfer(self.handle, &[0xFF, 0xFF]) {
            let raw = (u16::from(rx[0]) << 8) | u16::from(rx[1]);
            let id = vsig_id(&self.name, "read_angle").expect("valid vsig id");
            let _ = ctx.st.record(&id, Value::U32(u32::from(raw & 0x3FFF)));
        }
    }
    fn set_enabled(&mut self, on: bool, st: &mut StateTable) {
        if on {
            let _ = st.register(vsig_id(&self.name, "read_angle").expect("valid vsig id"), None);
        }
    }
}

/// Prove the DuplexTransfer primitive is engine-scoped and generic: two instantiation
/// -side test members couple over `spi:dial_initiator:cs` with **no FirmwareMember in the
/// transfer**. The initiator initiates mid-advance via [`MemberCtx::duplex_transfer`], the
/// responder answers synchronously from its own state, and the engine force-records the
/// exchange under the model-owned endpoint — identical machinery to the firmware path.
fn check_model_duplex(rep: &mut Report) {
    const ENDPOINT: &str = "spi:dial_initiator:cs";
    const STEP: u16 = 0x0100;

    let mut eng = Engine::new(TICK_US);
    // The responder is a shared member: added by value (idx 0), then linked to the bus
    // by its handle. It advances before the initiator (idx 1) reads each tick, so its
    // angle starts at 0x0000 and the initiator sees 0x0100, 0x0200, 0x0300.
    let responder = eng.add_member(DialResponder { name: "dial_responder".into(), angle: 0x0000, step: STEP });
    let handle = eng
        .link_duplex(ENDPOINT, responder)
        .expect("link the model responder peer");
    eng.add_member(DialInitiator { name: "dial_initiator".into(), handle });

    // The initiator reads the responder's just-advanced angle: 0x0100, 0x0200, 0x0300.
    for _ in 0..3 {
        eng.step().expect("engine step");
    }

    let read = eng
        .read("vsig:dial_initiator:read_angle")
        .ok()
        .flatten()
        .as_ref()
        .and_then(Value::as_u64);
    rep.check(
        "initiator reads the responder's frame synchronously (no firmware)",
        read == Some(0x0300),
        format!("vsig:dial_initiator:read_angle = {read:?} (expect {})", 0x0300),
    );

    // The engine force-recorded the exchange as :tx / :rx events under the endpoint
    // (one transfer/tick, never deduped -> three entries each).
    let tx_id = SignalId::parse(&format!("{ENDPOINT}:tx")).expect("valid spi id");
    let rx_id = SignalId::parse(&format!("{ENDPOINT}:rx")).expect("valid spi id");
    let n_tx = eng.state().changes(&tx_id).map(|c| c.len()).unwrap_or(0);
    let n_rx = eng.state().changes(&rx_id).map(|c| c.len()).unwrap_or(0);
    let last_rx = eng.state().current_value(&rx_id).ok().flatten();
    rep.check(
        "engine records model duplex as :tx/:rx events under the model-owned endpoint",
        (n_tx == 3) && (n_rx == 3) && (last_rx == Some(Value::Bytes(vec![0x03, 0x00]))),
        format!("{n_tx} tx + {n_rx} rx events; last rx = {last_rx:?}"),
    );
    rep.absorb(eng.take_logs());
}

/// Decode an AS5048 wire frame (big-endian: byte 0 = bits 15..8) into
/// `(parity_ok, error_flag, raw14)`. Even parity: the count of ones across all
/// 16 bits (parity bit included) must be even.
fn as5048_decode(frame: &[u8]) -> Option<(bool, bool, u16)> {
    if frame.len() != 2 {
        return None;
    }
    let f = u16::from_be_bytes([frame[0], frame[1]]);
    Some((f.count_ones().is_multiple_of(2), (f & 0x4000) != 0, f & 0x3FFF))
}

/// The AS5048 encoder model, standalone (no firmware): signal registration, the
/// unit boundary (`angle[deg]` in, canonical rad stored), quantization + wrap,
/// the pipelined READ-ANGLE wire frame, and the parity-error path.
fn check_as5048_model(rep: &mut Report) {
    const READ_ANGLE: [u8; 2] = [0xFF, 0xFF]; // parity 1, read 1, addr 0x3FFF
    const BAD_PARITY: [u8; 2] = [0x7F, 0xFF]; // 15 ones -> parity invalid

    let mut eng = Engine::new(TICK_US);
    let motor = eng.add_member(As5048Model::new("as5048_motor", 0.0));
    let dial = eng.add_member(As5048Model::new("dial", 0.0));

    // One `angle` signal per instance (canonical rad — units are a boundary
    // conversion, never part of the id) plus the raw-ticks output.
    let expected: Vec<String> = ["as5048_motor", "dial"]
        .iter()
        .flat_map(|m| {
            ["angle", "raw_encoder_ticks"]
                .iter()
                .map(move |s| format!("vsig:{m}:{s}"))
        })
        .collect();
    let missing: Vec<&str> = expected
        .iter()
        .filter(|id| !eng.state().signals().any(|s| s.as_str() == **id))
        .map(String::as_str)
        .collect();
    rep.check(
        "both AS5048 instances register the angle input + raw output",
        missing.is_empty(),
        if missing.is_empty() {
            format!("all {} vsig ids present", expected.len())
        } else {
            format!("missing: {missing:?}")
        },
    );

    if missing.is_empty() {
        // Command 90 deg through the unit boundary (canonical storage is rad);
        // one step folds it into the model and publishes the quantized output.
        let wrote = eng.write("vsig:as5048_motor:angle[deg]", 90.0);
        rep.check(
            "angle commanded through the unit boundary (angle[deg] = 90)",
            wrote.is_ok(),
            format!("write -> {wrote:?}"),
        );
        eng.step().expect("engine step");
        let raw = eng
            .read("vsig:as5048_motor:raw_encoder_ticks")
            .ok()
            .flatten()
            .as_ref()
            .and_then(Value::as_u64);
        rep.check(
            "model quantizes the commanded angle (90 deg -> 4096 counts, rounded)",
            raw == Some(4096),
            format!("raw_encoder_ticks = {raw:?} (expect 4096 = 16384/4)"),
        );

        // Two pipelined READ-ANGLE transfers: the response to command N arrives
        // in transfer N+1, so the second frame carries the angle.
        let (first, second) = {
            let mut m = motor.borrow_mut();
            (m.transfer(&READ_ANGLE), m.transfer(&READ_ANGLE))
        };
        let decoded = as5048_decode(&second);
        rep.check(
            "READ-ANGLE response decodes on the wire (parity ok, no error, raw 4096)",
            matches!(decoded, Some((true, false, 4096))),
            format!(
                "frames {first:02X?} then {second:02X?}; second -> (parity_ok, err, raw) = {decoded:?}"
            ),
        );

        // A negative command wraps into [0, 2pi) — and because the wrapped value
        // differs from the raw command, this also proves the model PUBLISHES its
        // folded angle back to the table (the signal is model state, not an echo
        // of the last write).
        let wrote = eng.write("vsig:as5048_motor:angle[deg]", -90.0);
        rep.check(
            "negative angle accepted through the unit boundary (angle[deg] = -90)",
            wrote.is_ok(),
            format!("write -> {wrote:?}"),
        );
        eng.step().expect("engine step");
        let wrapped_deg = eng
            .read("vsig:as5048_motor:angle[deg]")
            .ok()
            .flatten()
            .as_ref()
            .and_then(Value::as_f64);
        let raw_neg = eng
            .read("vsig:as5048_motor:raw_encoder_ticks")
            .ok()
            .flatten()
            .as_ref()
            .and_then(Value::as_u64);
        rep.check(
            "-90 deg wraps to 270 deg and 12288 counts (model publishes its state)",
            matches!(wrapped_deg, Some(d) if (d - 270.0).abs() < 0.05) && (raw_neg == Some(12288)),
            format!("angle[deg] reads {wrapped_deg:?} (expect ~270), raw = {raw_neg:?} (expect 12288)"),
        );

        // A parity-corrupt command must surface as the error flag in a later
        // response (on the dial instance, so the motor's state stays clean).
        let err_resp = {
            let mut d = dial.borrow_mut();
            let _ = d.transfer(&BAD_PARITY);
            d.transfer(&READ_ANGLE)
        };
        let decoded_err = as5048_decode(&err_resp);
        rep.check(
            "parity-corrupt command raises the error flag in a later response",
            matches!(decoded_err, Some((_, true, _))),
            format!("frame {err_resp:02X?} -> (parity_ok, err, raw) = {decoded_err:?}"),
        );
    } else {
        rep.check(
            "encoder transfer content",
            false,
            "skipped: angle signal not registered (fix the registration first)".into(),
        );
    }
    rep.absorb(eng.take_logs());
}

/// The sim `HW_TIM` publishes the firmware's commanded bridge state as output ports
/// — normalized per-phase duty, per-phase enable, and the master output enable (the
/// D6 route source a motor model consumes). The firmware boots the bridge dark, so
/// after a few ticks every port reads 0.0: registration + the dark boot state are
/// what this check pins. Live tracking waits on the stage-6/7 gate-driver bring-up
/// that lets drive engage, so it is deliberately not asserted here.
fn check_pwm_ports(fw: &Firmware, rep: &mut Report) {
    const PORTS: [&str; 7] = [
        "PWM_U_duty", "PWM_V_duty", "PWM_W_duty",
        "PWM_U_enabled", "PWM_V_enabled", "PWM_W_enabled",
        "TIM1_MOE",
    ];

    let mut eng = Engine::new(TICK_US);
    eng.add_member(FirmwareMember::new(SOURCE, fw, TICK_US));

    // Registered during sil_fw_start, applied to the table at add_member.
    let missing: Vec<String> = PORTS
        .iter()
        .map(|p| format!("vsig:{SOURCE}:{p}"))
        .filter(|id| !eng.state().signals().any(|s| s.as_str() == id))
        .collect();
    rep.check(
        "sim HW_TIM registers 7 PWM/bridge observation ports",
        missing.is_empty(),
        if missing.is_empty() {
            format!("all present: {PORTS:?}")
        } else {
            format!("missing: {missing:?}")
        },
    );

    // app_motorControl re-commands the dark bridge every tick, so the ports publish
    // 0 through the production setters — read them back after a few steps.
    for _ in 0..5 {
        eng.step().expect("engine step");
    }
    let wrong: Vec<String> = PORTS
        .iter()
        .filter_map(|p| {
            let id = format!("vsig:{SOURCE}:{p}");
            let v = eng.read(&id).ok().flatten().as_ref().and_then(Value::as_f64);
            match v {
                Some(0.0) => None,
                other => Some(format!("{p}={other:?}")),
            }
        })
        .collect();
    rep.check(
        "PWM/bridge ports read the dark-bridge boot state (duty/enable/MOE = 0)",
        wrong.is_empty(),
        if wrong.is_empty() {
            "all 7 ports = 0.0 (bridge dark)".into()
        } else {
            format!("non-zero: {wrong:?}")
        },
    );
    rep.absorb(eng.take_logs());
}

/// Phase-isolated performance report (informational — **not** pass/fail). Times each
/// phase over `N` ticks after a warm-up and prints µs/tick + ×realtime. `std::time` is
/// fine here — driver code, not sim-deterministic state. The phases isolate the cost:
///   1. firmware `advance_tick()` alone (no engine machinery);
///   2. full engine `step()` (a model + a `FirmwareMember` + a route);
///   3. empty engine `step()` (the engine's floor);
///   4. derived (`full - firmware`) — sweep+flush+ports+routes+table, not measured directly.
///
/// The report names the Rust profile and DLL flavor (`PCS_SIL_DLL_FLAVOR`) so a
/// copied-out table is self-describing.
fn report_performance(fw: &Firmware) {
    const WARMUP: u64 = 100;
    const N: u64 = 1000;

    // Registered-leaf count: enable a throwaway member on a scratch table.
    let leaves = {
        let mut st = StateTable::new();
        let mut fwm = FirmwareMember::new(SOURCE, fw, TICK_US);
        fwm.set_enabled(true, &mut st);
        fwm.cvar_leaf_count()
    };

    // (1) Firmware tick alone — raw advance_tick loop, no engine machinery.
    let fw_us = time_avg_us(WARMUP, N, || fw.advance_tick());

    // (2) Full engine step — the standard suite wiring: a model drives a firmware
    //     input cvar through a route while the FirmwareMember mirrors the whole
    //     namespace each tick (mirrors check_route_table).
    let full_us = {
        const STEP: u32 = 25; // stays within the u8 destination byte
        let src = vsig_id("sensor", "counts").expect("valid vsig id");
        let dst = SignalId::new("cvar", SOURCE, "HW_USB_sim_data.rx[0]", None)
            .expect("valid cvar id");
        let mut eng = Engine::new(TICK_US);
        eng.add_member(CountsRampModel::new("sensor", STEP));
        let mut fwm = FirmwareMember::new(SOURCE, fw, TICK_US);
        fwm.register_cvar_in_state_table("HW_USB_sim_data.rx[0]");
        eng.add_member(fwm);
        eng.add_route(src, dst).expect("add route");
        time_avg_us(WARMUP, N, || {
            eng.step().expect("engine step");
        })
    };

    // (3) Firmware-member-only step — a lone FirmwareMember mirroring the whole
    //     namespace (no model, no route): isolates the Tier-1 shadow sweep + sparse
    //     flush from the model/route overhead the full step adds.
    let sweep_us = {
        let mut eng = Engine::new(TICK_US);
        eng.add_member(FirmwareMember::new(SOURCE, fw, TICK_US));
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
    // Sweep-only cost over a bare firmware tick, and what model+route add on top.
    let sweep_over_fw_us = sweep_us - fw_us;
    let model_route_us = full_us - sweep_us;

    let rust_profile = if cfg!(debug_assertions) { "debug" } else { "release" };
    let dll_flavor = std::env::var("PCS_SIL_DLL_FLAVOR")
        .unwrap_or_else(|_| "unknown (build via tools/run_sil.sh)".into());
    // ×realtime = sim-time-per-tick / wall-time-per-tick = TICK_US / (µs/tick).
    let xrt = |us: f64| (TICK_US as f64) / us;

    println!("         Rust profile: {rust_profile}    firmware DLL: {dll_flavor}");
    println!("         {leaves} cvar leaves mirrored/tick    (sim tick = {TICK_US} µs, avg over {N} ticks)");
    println!("         phase                              µs/tick   ×realtime");
    println!("         firmware advance_tick alone        {fw_us:>7.2}   {:>6.1}×", xrt(fw_us));
    println!("         full engine step (measured)        {full_us:>7.2}   {:>6.1}×", xrt(full_us));
    println!("         firmware-member step (sweep+flush) {sweep_us:>7.2}   {:>6.1}×", xrt(sweep_us));
    println!("         empty engine step (floor)          {floor_us:>7.2}");
    println!("         derived: full - firmware           {derived_us:>7.2}   (sweep+flush+ports+routes+table)");
    println!("           of which shadow sweep+flush      {sweep_over_fw_us:>7.2}   (member step - firmware)");
    println!("           of which model+route+propagate   {model_route_us:>7.2}   (full step - member step)");
}

/// Warm up `body` for `warmup` iterations, then time `n` more and return the
/// mean wall-clock µs per iteration. Driver-side only (voyant core is
/// wall-clock-free).
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

/// Read the sim USB TX capture buffer (txLen + tx[] bytes) by DWARF and decode
/// it as the Teleplot text the firmware emitted.
fn read_tx_capture(fw: &Firmware) -> String {
    let len = fw.read_cvar("HW_USB_sim_data.txLen").as_u64().unwrap_or(0);
    let mut bytes = Vec::with_capacity(len as usize);
    for i in 0..len {
        let b = fw.read_cvar(&format!("HW_USB_sim_data.tx[{i}]")).as_u64().unwrap_or(0) as u8;
        bytes.push(b);
    }
    String::from_utf8_lossy(&bytes).into_owned()
}
