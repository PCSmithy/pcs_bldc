//! The whole-namespace cvar mirror is accurate and automatic: a firmware static no
//! other check touches both tracks (changes across the window) and equals a fresh
//! DWARF read of the same static — with no `sample_cvar` anywhere.

use pcs_bldc_sil::{cvar, Sil, SOURCE};

#[test]
fn mirror_tracks_firmware_memory() {
    let leaf = cvar("HW_ADC_data.tickCounter"); // the sim ADC's free-running counter
    let mut sim = Sil::new();
    let fwm = sim.load_firmware(SOURCE);
    sim.add_member(fwm);

    let mut vals: Vec<u64> = Vec::new();
    for _ in 0..6 {
        sim.step().expect("engine step");
        vals.push(sim.read_u64(leaf.as_str()));
    }
    // Single-threaded: the mirror at the end of the last tick equals firmware memory
    // now. `mem_now` is a direct DWARF read — the ground truth the mirror is checked
    // against.
    let table_now = sim.read_u64(leaf.as_str());
    let mem_now = sim.fw().read_cvar(leaf.name()).as_u64();
    let tracks = mem_now == Some(table_now);
    let changed = vals.windows(2).any(|w| w[0] != w[1]);
    assert!(
        tracks && changed,
        "cvar mirror tracks firmware memory (auto-derived): table {table_now} == memory {mem_now:?}; window {vals:?}"
    );
}
