//! Phase-2 driver: Rust loads the firmware shared library, drives it through
//! the control ABI, and reads/writes its state by symbol — including
//! non-exported statics, array elements, and a white-box *write* that the
//! firmware then reacts to (the Phase-2 exit: "step the firmware, read the ADC
//! ramp by symbol, write a global and see the firmware react").
//!
//! Usage: `cargo run -p pcs_bldc_sil -- [path-to-libpcs_bldc_fw.dll]`

use voyant::{Firmware, Value};
use std::path::PathBuf;

// The ADC sim ramp lives in a non-exported static struct; ch0/in6 is enabled.
const ADC_RAMP: &str = "HW_ADC_data.channelData[0].counts[6]";
const ADC_TICK: &str = "HW_ADC_data.tickCounter";

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

    // 1. Drive ticks; read the real ADC ramp value (array element, DWARF).
    println!("\n-- reading the ADC ramp by symbol (non-exported static array elem) --");
    for tick in 1..=6u32 {
        fw.advance_tick();
        let runs = fw.read_u32(b"sim_task1msRuns\0"); // exported global
        let ramp = fw.read(ADC_RAMP); // DWARF: a[i].b[j]
        println!("tick {tick:2}  sim_task1msRuns = {runs}   {ADC_RAMP} = {ramp}");
    }

    // 2. White-box WRITE: inject tickCounter, then advance — the firmware's next
    //    HW_ADC_run1ms recomputes the ramp from it. counts[6] = (96 + tickCounter).
    println!("\n-- writing {ADC_TICK} = 1000, then one tick --");
    fw.write(ADC_TICK, Value::U32(1000));
    fw.advance_tick();
    let tc = fw.read(ADC_TICK);
    let ramp = fw.read(ADC_RAMP);
    println!("after write+tick:  {ADC_TICK} = {tc}   {ADC_RAMP} = {ramp}");
    println!("(ramp jumped to ~1097 = 96 + 1001 — the firmware reacted to the Rust write)");

    fw.shutdown();
    println!("\ndone — Rust read non-exported statics/array elems and wrote firmware state via DWARF.");
}
