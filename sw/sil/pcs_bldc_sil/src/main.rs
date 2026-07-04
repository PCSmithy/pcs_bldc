//! pcs_bldc SIL sanity suite (and demo).
//!
//! Loads this board's firmware shared library, drives it over the control ABI, and proves —
//! purely through white-box DWARF read/write of firmware statics — that the REAL
//! firmware runs on the native scheduler: all four FreeRTOS tasks advance, the
//! State Table historian works, and one end-to-end data path
//! (task_1ms -> IO_AS5048 -> HW_SPI(sim) -> telemetryTask -> IO_serial ->
//! HW_USB(sim capture)) carries an injected encoder reading out as Teleplot text.
//!
//! Most checks now drive the firmware through the [`voyant::Engine`] step loop
//! (the sim clock): each `step` advances sim time, advances models, propagates
//! routes, runs the firmware, and samples registered cvars into the historian.
//! The engine is the suite's first real consumer. Checks 6 (route-table
//! primitives, read between propagate and advance_tick) and 1/7 (backend
//! lifecycle) stay below the engine on purpose.
//!
//! Each check prints PASS/FAIL; the process exits nonzero if any check fails, so
//! `tools/run_sil.sh` catches regressions. No firmware `_sim_*` API is called —
//! all injection/inspection is DWARF white-box (the sim drivers' statics are the
//! future State Table signals). It also exercises the model + Route Table seams:
//! a model's `vsig` is routed into a firmware `cvar` and read back, with
//! suspend/resume proving per-route fault-injection gating.
//!
//! Usage: `cargo run -p pcs_bldc_sil -- [path-to-firmware-shared-lib]`

use std::path::PathBuf;
use std::process::ExitCode;
use voyant::{
    record_model, register_model, vsig_id, Backend, Engine, Firmware, Model, ModelSignal,
    RampModel, RouteTable, SignalId, StateTable, Value,
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

/// Running tally of the sanity suite; prints each check and counts failures.
struct Report {
    failures: u32,
}

impl Report {
    fn new() -> Self {
        Report { failures: 0 }
    }

    fn check(&mut self, name: &str, pass: bool, detail: String) {
        if pass {
            println!("  [PASS] {name}\n         {detail}");
        } else {
            self.failures += 1;
            println!("  [FAIL] {name}\n         {detail}");
        }
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
    println!("\n-- 5. model-backed vsig signal (Model trait) --");
    check_model_vsig(&fw, &mut rep);

    // --- Check 6: Route Table drives a firmware cvar from a model -----------
    println!("\n-- 6. route table (model vsig -> firmware cvar) --");
    check_route_table(&fw, &mut rep);

    // --- Check 7: shutdown --------------------------------------------------
    println!("\n-- 7. shutdown --");
    fw.shutdown();
    rep.check("shutdown", true, "sil_fw_shutdown() returned cleanly".into());

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

    let mut eng = Engine::new(fw, TICK_US);
    let ids: Vec<SignalId> = COUNTERS.iter().map(|c| cvar(c)).collect();
    for id in &ids {
        eng.sample_cvar(id.clone(), None).expect("register counter");
    }
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
}

/// Drive the engine for a few ticks with a ramping ADC signal registered as a
/// sampled cvar; the engine records it into its historian each tick. Assert the
/// change-log accumulates and a ZOH historical lookup holds the prior sample;
/// read an enum cvar symbolically.
fn check_state_table(fw: &Firmware, rep: &mut Report) {
    let ramp = cvar("HW_ADC_data.channelData[0].counts[6]");
    let mut eng = Engine::new(fw, TICK_US);
    eng.sample_cvar(ramp.clone(), Some("counts")).unwrap();

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
    let mut eng = Engine::new(fw, TICK_US);
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
}

/// Demonstrate the `vsig` backing driven by the engine: a reference [`RampModel`]
/// is registered as a model, and the engine advances it with sim time and records
/// it through the same historian machinery as cvar samples each [`Engine::step`].
/// The firmware ticks alongside but is irrelevant to the model's own `vsig`.
fn check_model_vsig(fw: &Firmware, rep: &mut Report) {
    let mut eng = Engine::new(fw, TICK_US);
    eng.add_model(Box::new(RampModel::new("demo", 1000.0, Some("counts")))) // +1.0 / ms
        .expect("register model");
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
}

impl Model for CountsRampModel {
    fn name(&self) -> &str {
        &self.name
    }
    fn signals(&self) -> Vec<ModelSignal> {
        vec![ModelSignal::new("counts", Some("counts"))]
    }
    fn advance(&mut self, _dt_us: u64) {
        self.counts = self.counts.wrapping_add(self.step);
    }
    fn read(&self, local: &str) -> Option<Value> {
        match local {
            "counts" => Some(Value::U32(self.counts)),
            _ => None,
        }
    }
}

/// Prove the Route Table end-to-end against the real firmware: a Rust model's
/// `vsig` drives a firmware `cvar` (the Phase-3 shape — plant output into a
/// firmware sensor input), and suspend/resume gates that drive. Each tick:
/// advance + record the model, `propagate`, then read the firmware static back
/// (before any `advance_tick`, so the firmware ramp can't clobber it).
///
/// This one stays hand-rolled rather than driving the engine: it reads the
/// route-written `cvar` **between** `propagate` and `advance_tick` (the engine's
/// [`Engine::step`] bundles the two, and the firmware's own ADC ramp overwrites
/// `counts[6]` when it runs), and it toggles suspend/resume mid-sequence — a
/// finer granularity than a whole-tick step. It exercises the same [`RouteTable`]
/// the engine drives, just at the primitive level for the read-before-firmware
/// timing.
fn check_route_table(fw: &Firmware, rep: &mut Report) {
    let mut st = StateTable::new();
    let mut model = CountsRampModel::new("sensor", 100);
    register_model(&mut st, &model).expect("register vsig");
    let src = vsig_id("sensor", "counts").expect("valid vsig id");

    // Drive an ADC count static (a firmware sensor-input `cvar`) from the model.
    let dst = SignalId::new("cvar", SOURCE, "HW_ADC_data.channelData[0].counts[6]", None)
        .expect("valid cvar id");
    let mut routes = RouteTable::new();
    routes.add(src.clone(), dst.clone()).expect("add route");

    // Active route: the firmware static tracks the model exactly, tick by tick.
    let mut tracked = true;
    let mut last = 0u64;
    for tick in 1..=4u64 {
        model.advance(TICK_US);
        st.set_time(tick * TICK_US);
        record_model(&mut st, &model).expect("record vsig");
        routes.propagate(&st, fw).expect("propagate");
        let got = v_u64(&fw.read_cvar(dst.name())).unwrap_or(0);
        tracked &= got == (tick * 100);
        last = got;
    }
    rep.check(
        "route drives a firmware cvar from a model vsig",
        tracked && (last == 400),
        format!("counts[6] tracked the model to {last} over 4 ticks (expect 400)"),
    );

    // Suspend: advance the model to a new value; the firmware static must NOT
    // follow (the route stopped driving its destination).
    routes.suspend(&src, &dst).expect("suspend");
    model.advance(TICK_US);
    st.set_time(5 * TICK_US);
    record_model(&mut st, &model).expect("record vsig");
    routes.propagate(&st, fw).expect("propagate");
    let held = v_u64(&fw.read_cvar(dst.name())).unwrap_or(0);
    rep.check(
        "suspended route stops driving its destination",
        (held == last) && (held != 500),
        format!("counts[6] held at {held} while model advanced to 500"),
    );

    // Resume: the model's current value drives the destination again.
    routes.resume(&src, &dst).expect("resume");
    routes.propagate(&st, fw).expect("propagate");
    let resumed = v_u64(&fw.read_cvar(dst.name())).unwrap_or(0);
    rep.check(
        "resumed route drives the destination again",
        resumed == 500,
        format!("counts[6] = {resumed} after resume (expect 500)"),
    );
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
