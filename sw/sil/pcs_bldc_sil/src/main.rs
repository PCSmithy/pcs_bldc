//! pcs_bldc SIL sanity suite (and demo).
//!
//! Loads this board's firmware shared library, drives it over the control ABI, and proves —
//! purely through white-box DWARF read/write of firmware statics — that the REAL
//! firmware runs on the native scheduler: all four FreeRTOS tasks advance, the
//! State Table historian works, and one end-to-end data path
//! (task_1ms -> IO_AS5048 -> HW_SPI(sim) -> telemetryTask -> IO_serial ->
//! HW_USB(sim capture)) carries an injected encoder reading out as Teleplot text.
//!
//! All checks that exercise the sim now drive the firmware through the
//! [`voyant::Engine`] step loop (the sim clock): each `step` advances sim time,
//! advances models, propagates routes (table→table), runs the firmware, and
//! samples registered cvars into the historian. The engine is the suite's first
//! real consumer. Only checks 1/7 (backend lifecycle) stay below the engine.
//!
//! Each check prints PASS/FAIL; the process exits nonzero if any check fails, so
//! `tools/run_sil.sh` catches regressions. No firmware `_sim_*` API is called —
//! all injection/inspection is DWARF white-box (the sim drivers' statics are the
//! future State Table signals). It also exercises the model + Route Table seams on
//! the **real production path**: a model's `vsig` is routed into a firmware `cvar`
//! entry, a [`FirmwareMember`] flushes that entry into firmware memory, and the
//! value is read back — with suspend/resume proving per-route fault-injection
//! gating. The **port seam** is exercised end to end too: the sim ADC driver's
//! runtime-registered input ports carry a model's voltage (native format) into
//! the driver, which converts volts -> counts itself, while an undriven
//! neighbor input keeps its synthetic ramp. Warnings/errors logged into the
//! State Table during the run are drained and printed at the end
//! (informational).
//!
//! Usage: `cargo run -p pcs_bldc_sil -- [path-to-firmware-shared-lib]`

use std::path::PathBuf;
use std::process::ExitCode;
use std::time::Instant;
use voyant::{
    vsig_id, Engine, EngineError, Firmware, FirmwareMember, LogEntry, LogLevel, Member, RampModel,
    RouteError, SignalId, StateTable, Value,
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

/// Coerce a logical [`Value`] to an unsigned integer for the checks below.
fn v_u64(v: &Value) -> Option<u64> {
    match v {
        Value::U32(x) => Some(*x as u64),
        Value::U64(x) => Some(*x),
        Value::I32(x) => Some(*x as u64),
        Value::Bool(b) => Some(*b as u64),
        _ => None,
    }
}

/// Coerce a logical [`Value`] to a float for the checks below.
fn v_f64(v: &Value) -> Option<f64> {
    match v {
        Value::F32(x) => Some(*x as f64),
        Value::F64(x) => Some(*x),
        _ => None,
    }
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

    // --- Performance report (phase-isolated, informational) -----------------
    println!("\n-- performance report (phase-isolated, informational) --");
    report_performance(&fw);

    // --- Check 10: shutdown -------------------------------------------------
    println!("\n-- 10. shutdown --");
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

/// Read the four per-task heartbeat counters, advance ~50 ticks **through the
/// engine**, and assert each task advanced by roughly its expected count for its
/// period. The engine samples the counters into its historian each tick, so the
/// post-window values are read straight from the State Table.
fn check_tasks_advance(fw: &Firmware, rep: &mut Report) {
    const COUNTERS: [&str; 4] = ["task1msRuns", "task10msRuns", "taskUsbRuns", "telemRuns"];
    const N: u64 = 50;

    let before: Vec<u64> = COUNTERS
        .iter()
        .map(|c| v_u64(&fw.read_cvar(c)).unwrap_or(0))
        .collect();

    let mut eng = Engine::new(TICK_US);
    let ids: Vec<SignalId> = COUNTERS.iter().map(|c| cvar(c)).collect();
    // No per-signal declaration: the FirmwareMember auto-mirrors the whole cvar
    // namespace, so these counters are sampled into the historian each tick.
    eng.add_member(Box::new(FirmwareMember::new(SOURCE, fw, TICK_US)));
    for _ in 0..N {
        eng.step().expect("engine step");
    }
    // Post-window counter values come from the engine's own historian.
    let after: Vec<u64> = ids
        .iter()
        .map(|id| {
            eng.state()
                .current_value(id)
                .ok()
                .flatten()
                .and_then(v_u64)
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
    eng.add_member(Box::new(FirmwareMember::new(SOURCE, fw, TICK_US)));

    let mut samples = Vec::new();
    for _ in 1..=6u64 {
        eng.step().expect("engine step");
        let v = eng.state().current_value(&ramp).ok().flatten().cloned();
        samples.push(v.as_ref().and_then(v_u64).unwrap_or(0));
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

    // Enum-typed cvar resolves to its symbolic DWARF enumerator name.
    let tm = fw.read_cvar("HW_ADC_channelConfig[0].triggerMode");
    rep.check(
        "enum cvar reads as a symbolic name",
        matches!(tm, Value::Enum(_)),
        format!("HW_ADC_channelConfig[0].triggerMode = {tm}"),
    );
    rep.absorb(eng.take_logs());
}

/// The flagship check: drive the whole encoder->telemetry->USB path on the
/// native scheduler using only DWARF white-box writes, and read the resulting
/// Teleplot text back out of the sim USB TX capture.
fn check_end_to_end(fw: &Firmware, rep: &mut Report) {
    // (1) Open the port: IO_serial_connected(CDC) -> HW_USB_connected() reads
    //     HW_USB_sim_data.connected. Without this, telemetryTask skips its body.
    fw.write_cvar("HW_USB_sim_data.connected", &Value::Bool(true));
    let connected = matches!(fw.read_cvar("HW_USB_sim_data.connected"), Value::Bool(true));
    rep.check(
        "sim USB marked connected via DWARF write",
        connected,
        "HW_USB_sim_data.connected = true".into(),
    );

    // (2) Inject an AS5048 response on the MOTOR encoder's SPI channel
    //     (HW_SPI_CHANNEL_AS5048_1 = index 0). Response frame layout: bit15 =
    //     even parity, bit14 = error flag, bits[13:0] = angle. Frame 0x9000 has
    //     bits {15,12} set => even parity (valid), error flag clear, angle =
    //     0x1000 = 4096. The MOTOR channel is reverse=true, so the decoded raw
    //     is (16384 - 4096) = 12288 -> 270.00 deg. Sent big-endian: {0x90,0x00}.
    const CH: usize = 0; // HW_SPI_CHANNEL_AS5048_1
    fw.write_cvar(&format!("HW_SPI_data.channels[{CH}].injectedRx[0]"), &Value::U32(0x90));
    fw.write_cvar(&format!("HW_SPI_data.channels[{CH}].injectedRx[1]"), &Value::U32(0x00));
    fw.write_cvar(&format!("HW_SPI_data.channels[{CH}].injectedRxLen"), &Value::U64(2));

    // Drain any stale capture so we read a strictly post-injection window.
    fw.write_cvar("HW_USB_sim_data.txLen", &Value::U32(0));

    // (3) Advance through the engine: task_1ms samples the encoder each tick;
    //     telemetry fires every 2 ms. 10 ticks -> several windows. Injection and
    //     capture reads stay ad-hoc DWARF pokes on the shared firmware handle.
    //     The firmware runs as a FirmwareMember so the engine ticks it each step.
    let mut eng = Engine::new(TICK_US);
    eng.add_member(Box::new(FirmwareMember::new(SOURCE, fw, TICK_US)));
    for _ in 0..10 {
        eng.step().expect("engine step");
    }

    // (3a) The decoded encoder static reflects the injected SPI frame.
    let raw = v_u64(&fw.read_cvar(&format!("IO_AS5048_data.channels[{CH}].raw"))).unwrap_or(0);
    let deg = v_f64(&fw.read_cvar(&format!("IO_AS5048_data.channels[{CH}].angle_deg"))).unwrap_or(0.0);
    rep.check(
        "AS5048 decodes the injected SPI frame",
        (raw == 12288) && ((deg - 270.0).abs() < 0.05),
        format!("IO_AS5048_data.channels[0].raw = {raw} (expect 12288), angle_deg = {deg:.2} (expect 270.00)"),
    );

    // (3b) Pull the sim USB TX capture and confirm the Teleplot telemetry text.
    let text = read_tx_capture(fw);
    if let Some(first) = text.lines().next() {
        println!("         telemetry[0]: {first}");
    }
    let has_keys = text.contains("motor_angle:") && text.contains("motor_raw:");
    let has_angle = text.contains("270.00");
    let has_raw = text.contains("12288");
    rep.check(
        "telemetry text present with expected signal keys",
        has_keys,
        format!("captured {} bytes; motor_angle/motor_raw keys {}", text.len(),
                if has_keys { "present" } else { "MISSING" }),
    );
    rep.check(
        "telemetry carries the injected angle end-to-end (exact)",
        has_angle && has_raw,
        format!("text contains angle 270.00 = {has_angle}, raw 12288 = {has_raw}"),
    );

    // (4) Drain the capture (write txLen back to 0) and re-verify the next
    //     windows refill it — proves the path keeps flowing and the drain works.
    fw.write_cvar("HW_USB_sim_data.txLen", &Value::U32(0));
    for _ in 0..6 {
        eng.step().expect("engine step");
    }
    let text2 = read_tx_capture(fw);
    rep.check(
        "TX capture drains and refills across windows",
        text2.contains("270.00") && text2.contains("motor_raw:"),
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
    eng.add_member(Box::new(RampModel::new("demo", 1000.0, Some("counts")))); // +1.0 / ms
    // The firmware ticks alongside (irrelevant to the model's own vsig).
    eng.add_member(Box::new(FirmwareMember::new(SOURCE, fw, TICK_US)));
    let id = vsig_id("demo", "value").expect("valid vsig id");
    let registered = eng
        .state()
        .current_value(&id)
        .map(|v| v.is_none())
        .unwrap_or(false);
    rep.check(
        "model registers a vsig signal into the State Table",
        registered,
        format!("registered {} ({} signal(s) in table)", id, eng.state().len()),
    );

    for _ in 1..=5u64 {
        eng.step().expect("engine step");
    }
    let n_changes = eng.state().changes(&id).map(|c| c.len()).unwrap_or(0);
    let last = eng.state().current_value(&id).ok().flatten().cloned();
    rep.check(
        "vsig advances with sim time and the historian records it",
        (n_changes == 5) && matches!(&last, Some(Value::F64(v)) if (*v - 5.0).abs() < 1e-9),
        format!("{n_changes} change-log entries, current = {last:?} (expect F64(5.0))"),
    );
    rep.absorb(eng.take_logs());
}

/// A minimal integer "sensor" model for the route demo: one `counts` signal that
/// steps by a fixed amount each tick. It emits [`Value::U32`] so a route can drive
/// a firmware `uint32_t` static with no type conversion — voyant's [`RampModel`]
/// is `F64`, and a float→counts conversion is a *sensor model*'s job (deferred to
/// Phase 3), not a route's (routes are pure copies). Board-specific models like
/// this live on the instantiation side, per architecture.md §7.
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
    fn advance(&mut self, _dt_us: u64, st: &mut StateTable) {
        self.counts = self.counts.wrapping_add(self.step);
        let id = self.counts_id();
        if let Err(e) = st.record(&id, Value::U32(self.counts)) {
            st.log(LogLevel::Warning, &self.name, format!("record {id} failed: {e}"));
        }
    }
    fn set_enabled(&mut self, on: bool, st: &mut StateTable) {
        if on {
            let _ = st.register(self.counts_id(), Some("counts"));
        }
    }
}

/// Prove the Route Table end-to-end against the real firmware **on the engine**,
/// exercising the real production path: a model's `vsig` is recorded, a route
/// propagates it table→table into a firmware `cvar` entry, and a
/// [`FirmwareMember`] flushes that entry into firmware memory before `advance_tick`
/// — the Phase-3 shape (plant output into a firmware sensor input). Suspend/resume
/// gates the route.
///
/// The destination is the SPI sim's per-channel `injectedRx[0]` byte — a firmware
/// input the firmware *reads* (in `HW_SPI_private_fillRx`, when delivering a
/// crafted MISO response) but **never writes**, so a value flushed just before
/// `advance_tick` survives the tick and can be asserted afterward. (The old
/// destination, an ADC `counts[]` static, was overwritten by the firmware's own
/// ADC ramp each tick — which is exactly why the check formerly had to read
/// *between* propagate and advance_tick; table-mediated propagation folds the flush
/// inside the firmware member's advance, so that dodge is gone.) The model steps by
/// 25 to stay within the byte's range.
///
/// Member order `[model, firmware]` gives zero-lag tracking: the model records its
/// vsig, the pre-firmware `propagate` records it into the cvar entry, and the
/// firmware member flushes it in the same step.
fn check_route_table(fw: &Firmware, rep: &mut Report) {
    const STEP: u32 = 25; // stays within a u8 destination byte
    let src = vsig_id("sensor", "counts").expect("valid vsig id");
    // The SPI sim's injected-MISO byte: read by the firmware, never written by it.
    let dst = SignalId::new("cvar", SOURCE, "HW_SPI_data.channels[0].injectedRx[0]", None)
        .expect("valid cvar id");

    let mut eng = Engine::new(TICK_US);
    eng.add_member(Box::new(CountsRampModel::new("sensor", STEP)));
    let mut fwm = FirmwareMember::new(SOURCE, fw, TICK_US);
    // injectedRx is a 256-byte buffer, over the array threshold, so the driven
    // element is force-included to register + mirror it. No `drive_cvar`: a
    // registered cvar the framework command-writes (the route) is flushed by the
    // member automatically (fresh-dirty flush).
    fwm.include("HW_SPI_data.channels[0].injectedRx[0]");
    eng.add_member(Box::new(fwm));
    eng.add_route(src.clone(), dst.clone()).expect("add route");

    // Active route: firmware memory tracks the model exactly, step by step.
    let mut tracked = true;
    let mut last = 0u64;
    for tick in 1..=4u64 {
        eng.step().expect("engine step");
        let got = v_u64(&fw.read_cvar(dst.name())).unwrap_or(0);
        tracked &= got == (tick * STEP as u64);
        last = got;
    }
    rep.check(
        "route drives a firmware cvar from a model vsig (via FirmwareMember flush)",
        tracked && (last == 4 * STEP as u64),
        format!("injectedRx[0] tracked the model to {last} over 4 steps (expect {})", 4 * STEP),
    );

    // Suspend: the model keeps advancing, but the route stops recording the dest
    // entry, so the firmware member re-flushes the held value — firmware memory
    // must NOT follow the model.
    eng.suspend_route(&src, &dst).expect("suspend");
    eng.step().expect("engine step");
    let held = v_u64(&fw.read_cvar(dst.name())).unwrap_or(0);
    let model_now = eng
        .state()
        .current_value(&src)
        .ok()
        .flatten()
        .and_then(v_u64)
        .unwrap_or(0);
    rep.check(
        "suspended route stops driving its destination",
        (held == last) && (model_now > held),
        format!("injectedRx[0] held at {held} while model advanced to {model_now}"),
    );

    // Resume: the destination jumps to the model's current value again.
    eng.resume_route(&src, &dst).expect("resume");
    eng.step().expect("engine step");
    let resumed = v_u64(&fw.read_cvar(dst.name())).unwrap_or(0);
    let model_after = eng
        .state()
        .current_value(&src)
        .ok()
        .flatten()
        .and_then(v_u64)
        .unwrap_or(0);
    rep.check(
        "resumed route drives the destination again",
        resumed == model_after,
        format!("injectedRx[0] = {resumed} after resume (expect current model {model_after})"),
    );
    rep.absorb(eng.take_logs());
}

/// A model member for the feedback loop: reads input `in` (a firmware counter,
/// delivered on the *delayed* backward edge) and emits `out = in % 200` (a
/// [`Value::U32`] within a `u8` so it can drive the SPI sim's `injectedRx[0]` byte
/// with no conversion). The modulo keeps `out` in range regardless of the firmware
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
    fn advance(&mut self, _dt_us: u64, st: &mut StateTable) {
        let input = st
            .current_value(&self.in_id())
            .ok()
            .flatten()
            .and_then(v_u64)
            .unwrap_or(0);
        self.out = (input % 200) as u32;
        let id = self.out_id();
        if let Err(e) = st.record(&id, Value::U32(self.out)) {
            st.log(LogLevel::Warning, &self.name, format!("record {id} failed: {e}"));
        }
    }
    fn set_enabled(&mut self, on: bool, st: &mut StateTable) {
        if on {
            let _ = st.register(self.in_id(), None);
            let _ = st.register(self.out_id(), Some("counts"));
        }
    }
}

/// Prove the settled per-route latency design end-to-end against the real firmware,
/// as a genuine **two-member feedback loop**:
///
/// - forward (zero-latency): the model's `out` drives the firmware's SPI-sim input
///   byte `HW_SPI_data.channels[0].injectedRx[0]` (a cvar the firmware *reads* but
///   never writes, so a flushed value survives the tick);
/// - backward (DELAYED, the explicit ZOH cut): the firmware counter
///   `task1msRuns` routes back into the model's `in`.
///
/// The member order is `[model, firmware]`, so the backward firmware→model edge is a
/// backward edge in registration order: the validator **rejects** it while it is
/// zero-latency, and accepts it once declared delayed. We catch that step error,
/// fix the wiring **live** (rewire-at-runtime), then assert the exact deterministic
/// sequence the delayed loop produces.
fn check_feedback_loop(fw: &Firmware, rep: &mut Report) {
    let out = vsig_id("loop_model", "out").expect("valid vsig id");
    let inp = vsig_id("loop_model", "in").expect("valid vsig id");
    let counter = cvar("task1msRuns"); // firmware output (sampled)
    let miso = cvar("HW_SPI_data.channels[0].injectedRx[0]"); // firmware input (driven)

    let mut eng = Engine::new(TICK_US);
    eng.add_member(Box::new(LoopModel::new("loop_model"))); // idx 0
    let mut fwm = FirmwareMember::new(SOURCE, fw, TICK_US); // idx 1
    // The MISO byte lives in a 256-byte buffer (over threshold) -> force-include
    // it so the route can drive it. `task1msRuns` (the sampled counter) is a
    // top-level scalar, auto-mirrored with no declaration.
    fwm.include("HW_SPI_data.channels[0].injectedRx[0]");
    eng.add_member(Box::new(fwm));

    // Forward edge: model out -> firmware MISO byte (zero-latency, model before fw).
    eng.add_route(out.clone(), miso.clone()).expect("add forward route");
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

    // Predicted deterministic sequence for injectedRx[0] read after each step:
    //   step 1: model in is unset (fw counter not yet sampled by this engine) -> 0.
    //   step k>=2: in = task1msRuns as of the end of step k-1 = base + (k-1),
    //             so injectedRx[0] = (base + (k-1)) % 200.
    // `base` is the firmware counter right before the first successful step (the
    // rejected step returned early and did NOT advance the firmware).
    let base = v_u64(&fw.read_cvar(counter.name())).unwrap_or(0);
    const N: u64 = 6;
    let got: Vec<u64> = (1..=N)
        .map(|_| {
            eng.step().expect("engine step");
            v_u64(&fw.read_cvar(miso.name())).unwrap_or(0)
        })
        .collect();
    let predicted: Vec<u64> = (1..=N)
        .map(|k| if k == 1 { 0 } else { (base + (k - 1)) % 200 })
        .collect();

    rep.check(
        "delayed feedback loop produces the exact predicted sequence",
        got == predicted,
        format!("injectedRx[0] over {N} steps = {got:?}, predicted {predicted:?} (base counter {base})"),
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
    fn advance(&mut self, _dt_us: u64, st: &mut StateTable) {
        let id = self.volts_id();
        if let Err(e) = st.record(&id, Value::F64(self.volts)) {
            st.log(LogLevel::Warning, &self.name, format!("record {id} failed: {e}"));
        }
    }
    fn set_enabled(&mut self, on: bool, st: &mut StateTable) {
        if on {
            let _ = st.register(self.volts_id(), Some("V"));
        }
    }
}

/// Prove the **port seam** end to end on the idiomatic native-format path: the
/// sim ADC driver registered one input port per enabled input (during
/// `sil_fw_start`, via the control-ABI hook vtable), local names from the
/// channel config's per-input name strings (`ADC1_IN1`, ..., unit V). A model
/// member emits a *voltage*; a zero-latency route carries it — volts, native
/// format, member to member — into the firmware's port entry
/// `vsig:pcs_bldc:ADC1_IN6`; the [`FirmwareMember`] caches it for the C side;
/// and the sim ADC driver converts volts -> counts with its own numBits/vref
/// parameters (the conversion lives where real hardware does it). DWARF-read
/// the counts static and assert the exact quantization; a NEIGHBORING undriven
/// input must keep the synthetic ramp (the fallback, proven in the same run).
fn check_adc_ports(fw: &Firmware, rep: &mut Report) {
    const VOLTS: f64 = 1.234;
    // ADC1 regular input 6 (port ADC1_IN6) is driven; input 1 stays undriven.
    let port = SignalId::new("vsig", SOURCE, "ADC1_IN6", None).expect("valid port id");
    let driven_counts = "HW_ADC_data.channelData[0].counts[6]";
    let neighbor_counts = "HW_ADC_data.channelData[0].counts[1]";

    let mut eng = Engine::new(TICK_US);
    eng.add_member(Box::new(VoltsModel::new("pin_model", VOLTS)));
    eng.add_member(Box::new(FirmwareMember::new(SOURCE, fw, TICK_US)));

    // The ADC's ports were registered during sil_fw_start; the FirmwareMember
    // applied them to the table at add_member. The board config enables 10
    // regular inputs (5 per ADC), so 10 ports exist under this member's name.
    let n_ports = eng
        .state()
        .signals()
        .filter(|s| (s.sig_type() == "vsig") && (s.source() == SOURCE))
        .count();
    rep.check(
        "sim ADC registered one input port per enabled input",
        n_ports == 10,
        format!("{n_ports} vsig:{SOURCE}:* port(s) in the table (expect 10)"),
    );

    // Route the model's volts into the port (native format: volts -> volts).
    eng.add_route(vsig_id("pin_model", "volts").expect("valid vsig id"), port.clone())
        .expect("add route");

    // Expected counts: mirror of the sim driver's volts->counts math
    // (HW_ADC_private_voltsToCounts), with numBits/vref read via DWARF from
    // the sim channel config rather than hardcoded.
    let vref = v_f64(&fw.read_cvar("HW_ADC_channelConfig[0].vref")).unwrap_or(0.0);
    let num_bits = v_u64(&fw.read_cvar("HW_ADC_channelConfig[0].numBits")).unwrap_or(0);
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
        driven.push(v_u64(&fw.read_cvar(driven_counts)).unwrap_or(0));
        neighbor.push(v_u64(&fw.read_cvar(neighbor_counts)).unwrap_or(0));
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
    eng.add_member(Box::new(FirmwareMember::new(SOURCE, fw, TICK_US)));

    let mut vals: Vec<Option<u64>> = Vec::new();
    for _ in 0..6 {
        eng.step().expect("engine step");
        vals.push(eng.state().current_value(&leaf).ok().flatten().and_then(v_u64));
    }
    // The mirror at the end of the last tick equals firmware memory now (single-
    // threaded: nothing changed memory between the sweep and this read).
    let table_now = eng.state().current_value(&leaf).ok().flatten().and_then(v_u64);
    let mem_now = v_u64(&fw.read_cvar(leaf.name()));
    let tracks = table_now.is_some() && (table_now == mem_now);
    let changed = vals.windows(2).any(|w| w[0] != w[1]);
    rep.check(
        "cvar mirror tracks firmware memory (auto-derived, no declaration)",
        tracks && changed,
        format!("table {table_now:?} == memory {mem_now:?}; window {vals:?}"),
    );
    rep.absorb(eng.take_logs());
}

/// Phase-isolated performance report (informational — **not** pass/fail). Times
/// three phases, each averaged over `N` ticks after a warm-up, and prints
/// µs/tick + implied ×realtime for the full step. `std::time` is fine here —
/// this is driver code, not sim-deterministic state (voyant core stays
/// wall-clock-free). The phases isolate where the per-tick cost lives:
///
///   1. **firmware `advance_tick()` alone** — a raw loop, no engine machinery.
///   2. **full engine `step()`** — the standard suite wiring (a model + a
///      `FirmwareMember` mirroring the whole cvar namespace + a route).
///   3. **empty engine `step()`** — no members, no routes: the engine's own floor.
///   4. **derived** (`full - firmware`) ≈ sweep + flush + ports + routes + table
///      cost — *derived*, not measured directly.
///
/// The report names which Rust profile built the driver (`cfg!(debug_assertions)`)
/// and which optimization built the DLL (`PCS_SIL_DLL_FLAVOR`, set by run_sil.sh),
/// so a copied-out table is self-describing.
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
        let dst = SignalId::new("cvar", SOURCE, "HW_SPI_data.channels[0].injectedRx[0]", None)
            .expect("valid cvar id");
        let mut eng = Engine::new(TICK_US);
        eng.add_member(Box::new(CountsRampModel::new("sensor", STEP)));
        let mut fwm = FirmwareMember::new(SOURCE, fw, TICK_US);
        fwm.include("HW_SPI_data.channels[0].injectedRx[0]");
        eng.add_member(Box::new(fwm));
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
        eng.add_member(Box::new(FirmwareMember::new(SOURCE, fw, TICK_US)));
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
    let len = v_u64(&fw.read_cvar("HW_USB_sim_data.txLen")).unwrap_or(0);
    let mut bytes = Vec::with_capacity(len as usize);
    for i in 0..len {
        let b = v_u64(&fw.read_cvar(&format!("HW_USB_sim_data.tx[{i}]"))).unwrap_or(0) as u8;
        bytes.push(b);
    }
    String::from_utf8_lossy(&bytes).into_owned()
}
