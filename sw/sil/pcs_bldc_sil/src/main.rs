//! pcs_bldc instantiation of the voyant framework.
//!
//! Loads this board's firmware, registers signals in a State Table, and each
//! tick samples firmware state (via the cvar resolver) into the table —
//! demonstrating the historian: change-logged per-signal history + a current
//! cache, addressed by canonical `SignalId`.
//!
//! Usage: `cargo run -p pcs_bldc_sil -- [path-to-libpcs_bldc_fw.dll]`

use std::path::PathBuf;
use voyant::{Firmware, SignalId, StateTable, Value};

const SOURCE: &str = "pcs_bldc";
const TICK_US: u64 = 1_000; // the firmware's 1 ms task cadence

/// Canonical `cvar` id for a firmware DWARF path on this board.
fn cvar(local: &str) -> SignalId {
    SignalId::new("cvar", SOURCE, local, None).expect("valid cvar id")
}

fn default_dll_path() -> PathBuf {
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

    // Register three signals: a ramp (changes every tick), a counter (changes
    // every tick), and a constant flag (to show change-logging dedup).
    let mut st = StateTable::new();
    let ramp = cvar("HW_ADC_data.channelData[0].counts[6]");
    let tickc = cvar("HW_ADC_data.tickCounter");
    let inited = cvar("HW_ADC_data.initialized");
    st.register(ramp.clone(), Some("counts")).unwrap();
    st.register(tickc.clone(), Some("counts")).unwrap();
    st.register(inited.clone(), None).unwrap();
    println!("State Table: {} signals registered\n", st.len());

    // Sample the firmware into the table each tick (the "record" step).
    println!("-- sampling firmware into the State Table each tick --");
    for tick in 1..=8u64 {
        fw.advance_tick();
        st.set_time(tick * TICK_US);
        st.record(&ramp, fw.read_cvar(ramp.name())).unwrap();
        st.record(&tickc, fw.read_cvar(tickc.name())).unwrap();
        st.record(&inited, fw.read_cvar(inited.name())).unwrap();
        println!(
            "t={:>5}us  {ramp} = {}",
            tick * TICK_US,
            st.current_value(&ramp).unwrap().unwrap()
        );
    }

    // The historian: dump the per-signal change-log, and show dedup.
    println!("\n-- {ramp} change-log (changes every tick) --");
    for (t, v) in st.changes(&ramp).unwrap() {
        println!("  t={t:>5}us  {v}");
    }
    println!(
        "\n{inited} change-log has {} sample(s) — constant value deduped by change-logging",
        st.changes(&inited).unwrap().len()
    );

    // Zero-order-hold historical lookup.
    let mid = 3 * TICK_US + 500;
    println!(
        "value_at({mid}us) of {ramp} = {} (ZOH — held from tick 3)",
        st.value_at(&ramp, mid).unwrap().unwrap()
    );

    // Enum-typed cvars resolve to their symbolic name (DWARF enumerators).
    println!("\n-- enum cvars read as symbolic names --");
    for local in [
        "HW_ADC_channelConfig[0].triggerMode",
        "HW_ADC_channelConfig[0].xferMode",
    ] {
        println!("  {local} = {}", fw.read_cvar(local));
    }

    // White-box write through the resolver; the firmware reacts on next tick.
    println!("\n-- inject: write {tickc} = 1000, one tick --");
    fw.write_cvar(tickc.name(), &Value::U32(1000));
    fw.advance_tick();
    st.set_time(9 * TICK_US);
    st.record(&ramp, fw.read_cvar(ramp.name())).unwrap();
    st.record(&tickc, fw.read_cvar(tickc.name())).unwrap();
    println!(
        "after write+tick:  {tickc} = {}   {ramp} = {}",
        st.current_value(&tickc).unwrap().unwrap(),
        st.current_value(&ramp).unwrap().unwrap()
    );

    fw.shutdown();
    println!("\ndone — firmware sampled into the State Table historian by canonical SignalId.");
}
