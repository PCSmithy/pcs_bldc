//! The State Table historian: it records a changing ADC ramp, holds prior samples
//! under a zero-order-hold lookup, and reads an enum cvar as its symbolic name.

use pcs_bldc_sil::{cid, cvar, Sil, TICK_US};
use voyant::Value;

#[test]
fn historian_zoh_and_enum() {
    let mut world = Sil::new();
    let ramp = cvar("HW_ADC_data.channelData[0].counts[6]");
    // Auto-mirrored: counts[19] is under the array threshold, so counts[6] is
    // registered + sampled with no `sample_cvar` declaration.
    let fwm = world.firmware_member();
    world.sim.add_member(fwm);

    let mut samples = Vec::new();
    for _ in 1..=6u64 {
        world.sim.step().expect("engine step");
        let v = world.sim.read(ramp.as_str()).ok().flatten();
        samples.push(v.as_ref().and_then(Value::as_u64).unwrap_or(0));
    }

    let changed = samples.windows(2).any(|w| w[0] != w[1]);
    let n_changes = world.sim.state().changes(&ramp).unwrap().len();
    assert!(
        changed && (n_changes >= 2),
        "historian records a changing ADC ramp: counts[6] samples {samples:?}, {n_changes} change-log entries"
    );

    // Zero-order-hold: a lookup between samples holds the prior value.
    let mid = (2 * TICK_US) + 500;
    let zoh = world.sim.state().value_at(&ramp, mid).unwrap();
    assert!(zoh.is_some(), "ZOH historical lookup at {mid}us = {zoh:?}");

    // Enum-typed cvar resolves to its symbolic DWARF enumerator name (auto-mirrored;
    // the boxed lane carries the Enum value after stepping).
    let tm = world
        .sim
        .read(&cid("HW_ADC_channelConfig[0].triggerMode"))
        .ok()
        .flatten()
        .unwrap_or(Value::U32(0));
    assert!(
        matches!(tm, Value::Enum(_)),
        "enum cvar reads as a symbolic name: HW_ADC_channelConfig[0].triggerMode = {tm}"
    );
}
