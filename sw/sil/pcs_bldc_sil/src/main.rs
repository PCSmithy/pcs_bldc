//! pcs_bldc instantiation of the voyant framework.
//!
//! Loads this board's firmware, builds a State Table over it, and reads/writes
//! firmware state **by canonical name** (`cvar:pcs_bldc:<dwarf-path>`) — the
//! same white-box loop as before, now through the framework's State Table.
//!
//! Usage: `cargo run -p pcs_bldc_sil -- [path-to-libpcs_bldc_fw.dll]`

use std::path::PathBuf;
use voyant::{Firmware, StateTable, Value};

const SOURCE: &str = "pcs_bldc";

/// Canonical `cvar` key for a firmware DWARF path on this board.
fn cvar(local: &str) -> String {
    format!("cvar:{SOURCE}:{local}")
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

    // Build the State Table over the firmware and register the signals we care
    // about by canonical name.
    let mut st = StateTable::new(&fw);
    let ramp = cvar("HW_ADC_data.channelData[0].counts[6]"); // non-exported static, array elem
    let tick_counter = cvar("HW_ADC_data.tickCounter");
    st.register(&ramp).expect("register ramp");
    st.register(&tick_counter).expect("register tickCounter");
    println!("State Table: {} entries registered\n", st.len());

    // 1. Drive ticks; read the ADC ramp through the State Table.
    println!("-- reading the ADC ramp through the State Table --");
    for tick in 1..=6u32 {
        fw.advance_tick();
        println!("tick {tick:2}  {ramp} = {}", st.read(&ramp));
    }

    // 2. Inject by writing through the State Table; the firmware reacts.
    println!("\n-- writing {tick_counter} = 1000, then one tick --");
    st.write(&tick_counter, Value::U32(1000));
    fw.advance_tick();
    println!(
        "after write+tick:  {tick_counter} = {}   {ramp} = {}",
        st.read(&tick_counter),
        st.read(&ramp)
    );
    println!("(ramp jumped to ~1097 — the firmware reacted to a write through the State Table)");

    fw.shutdown();
    println!("\ndone — firmware state read/written by canonical name through the State Table.");
}
