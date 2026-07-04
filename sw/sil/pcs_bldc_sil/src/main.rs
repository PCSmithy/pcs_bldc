//! pcs_bldc SIL sanity suite (and demo).
//!
//! Loads this board's firmware shared library, drives it over the control ABI, and proves —
//! purely through white-box DWARF read/write of firmware statics — that the REAL
//! firmware runs on the native scheduler: all four FreeRTOS tasks advance, the
//! State Table historian works, and one end-to-end data path
//! (task_1ms -> IO_AS5048 -> HW_SPI(sim) -> telemetryTask -> IO_serial ->
//! HW_USB(sim capture)) carries an injected encoder reading out as Teleplot text.
//!
//! Each check prints PASS/FAIL; the process exits nonzero if any check fails, so
//! `tools/run_sil.sh` catches regressions. No firmware `_sim_*` API is called —
//! all injection/inspection is DWARF white-box (the sim drivers' statics are the
//! future State Table signals).
//!
//! Usage: `cargo run -p pcs_bldc_sil -- [path-to-firmware-shared-lib]`

use std::path::PathBuf;
use std::process::ExitCode;
use voyant::{
    record_model, register_model, vsig_id, Backend, Firmware, Model, RampModel, SignalId,
    StateTable, Value,
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
    check_model_vsig(&mut rep);

    // --- Check 6: shutdown --------------------------------------------------
    println!("\n-- 6. shutdown --");
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

/// Read the four per-task heartbeat counters, advance ~50 ticks, and assert each
/// task advanced by roughly its expected count for its period.
fn check_tasks_advance(fw: &Firmware, rep: &mut Report) {
    const COUNTERS: [&str; 4] = ["task1msRuns", "task10msRuns", "taskUsbRuns", "telemRuns"];
    const N: u64 = 50;

    let before: Vec<u64> = COUNTERS
        .iter()
        .map(|c| v_u64(&fw.read_cvar(c)).unwrap_or(0))
        .collect();
    for _ in 0..N {
        fw.advance_tick();
    }
    let after: Vec<u64> = COUNTERS
        .iter()
        .map(|c| v_u64(&fw.read_cvar(c)).unwrap_or(0))
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

/// Sample a ramping ADC signal into the State Table across ticks and assert it
/// change-logs; do a ZOH historical lookup; read an enum cvar symbolically.
fn check_state_table(fw: &Firmware, rep: &mut Report) {
    let mut st = StateTable::new();
    let ramp = SignalId::new("cvar", SOURCE, "HW_ADC_data.channelData[0].counts[6]", None)
        .expect("valid cvar id");
    st.register(ramp.clone(), Some("counts")).unwrap();

    let mut samples = Vec::new();
    for tick in 1..=6u64 {
        fw.advance_tick();
        st.set_time(tick * TICK_US);
        let v = fw.read_cvar(ramp.name());
        st.record(&ramp, v.clone()).unwrap();
        samples.push(v_u64(&v).unwrap_or(0));
    }

    let changed = samples.windows(2).any(|w| w[0] != w[1]);
    let n_changes = st.changes(&ramp).unwrap().len();
    rep.check(
        "historian records a changing ADC ramp signal",
        changed && (n_changes >= 2),
        format!("counts[6] samples {samples:?}, {n_changes} change-log entries"),
    );

    // Zero-order-hold: a lookup between samples holds the prior value.
    let mid = (2 * TICK_US) + 500;
    let zoh = st.value_at(&ramp, mid).unwrap();
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

    // (3) Advance: task_1ms samples the encoder each tick; telemetry fires every
    //     2 ms. 10 ticks -> several windows.
    for _ in 0..10 {
        fw.advance_tick();
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
        fw.advance_tick();
    }
    let text2 = read_tx_capture(fw);
    rep.check(
        "TX capture drains and refills across windows",
        text2.contains("270.00") && text2.contains("motor_raw:"),
        format!("post-drain capture {} bytes, still carries the angle", text2.len()),
    );
}

/// Demonstrate the `vsig` backing: a reference [`RampModel`] registers into the
/// State Table, advances with sim time, and is recorded through the same
/// historian machinery as cvar samples (no firmware involved — models are a
/// separate, Rust-side backing).
fn check_model_vsig(rep: &mut Report) {
    let mut st = StateTable::new();
    let mut model = RampModel::new("demo", 1000.0, Some("counts")); // +1.0 / ms

    register_model(&mut st, &model).expect("register vsig");
    let id = vsig_id(model.name(), "value").expect("valid vsig id");
    let registered = st.current_value(&id).map(|v| v.is_none()).unwrap_or(false);
    rep.check(
        "model registers a vsig signal into the State Table",
        registered,
        format!("registered {} ({} signal(s) in table)", id, st.len()),
    );

    for tick in 1..=5u64 {
        model.advance(TICK_US);
        st.set_time(tick * TICK_US);
        record_model(&mut st, &model).expect("record vsig");
    }
    let n_changes = st.changes(&id).map(|c| c.len()).unwrap_or(0);
    let last = st.current_value(&id).ok().flatten().cloned();
    rep.check(
        "vsig advances with sim time and the historian records it",
        (n_changes == 5) && matches!(&last, Some(Value::F64(v)) if (*v - 5.0).abs() < 1e-9),
        format!("{n_changes} change-log entries, current = {last:?} (expect F64(5.0))"),
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
