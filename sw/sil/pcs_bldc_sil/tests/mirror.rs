//! The whole-namespace cvar mirror is accurate and automatic: a firmware static no
//! other check touches both tracks (changes across the window) and equals a fresh
//! DWARF read of the same static — with no `sample_cvar` anywhere.

use pcs_bldc_sil::{cvar, Sil};
use voyant::Value;

#[test]
fn mirror_tracks_firmware_memory() {
    let leaf = cvar("HW_ADC_data.tickCounter"); // the sim ADC's free-running counter
    let mut world = Sil::new();
    let fwm = world.firmware_member();
    world.sim.add_member(fwm);

    let mut vals: Vec<Option<u64>> = Vec::new();
    for _ in 0..6 {
        world.sim.step().expect("engine step");
        vals.push(world.sim.read(leaf.as_str()).ok().flatten().as_ref().and_then(Value::as_u64));
    }
    // Single-threaded: the mirror at the end of the last tick equals firmware memory
    // now. `mem_now` is a direct DWARF read — the ground truth the mirror is checked
    // against.
    let table_now = world.sim.read(leaf.as_str()).ok().flatten().as_ref().and_then(Value::as_u64);
    let mem_now = world.fw().read_cvar(leaf.name()).as_u64();
    let tracks = table_now.is_some() && (table_now == mem_now);
    let changed = vals.windows(2).any(|w| w[0] != w[1]);
    assert!(
        tracks && changed,
        "cvar mirror tracks firmware memory (auto-derived): table {table_now:?} == memory {mem_now:?}; window {vals:?}"
    );
}
