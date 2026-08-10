//! A model member's `vsig` is driven by the engine and recorded through the same
//! historian machinery as cvar samples; the firmware ticks alongside, irrelevant to
//! the model's own signal.

use pcs_bldc_sil::{Sil, SOURCE};
use voyant::{vsig_id, RampModel};

#[test]
fn vsig_ramp_advances_and_records() {
    let mut sim = Sil::new();
    sim.add_member(RampModel::new("demo", 1000.0, Some("counts"))); // +1.0 / ms

    // The firmware ticks alongside (irrelevant to the model's own vsig).
    let fwm = sim.load_firmware(SOURCE);
    sim.add_member(fwm);

    let id = vsig_id("demo", "value").expect("valid vsig id");
    // Registered by the model at add, but not yet recorded (read -> Ok(None)).
    let registered = sim.read(id.as_str()).map(|v| v.is_none()).unwrap_or(false);
    assert!(
        registered,
        "model registers {id} into the State Table ({} signal(s))",
        sim.state().len()
    );

    for _ in 1..=5u64 {
        sim.step().expect("engine step");
    }
    let n_changes = sim.state().changes(&id).map(|c| c.len()).unwrap_or(0);
    let last = sim.read_f64(id.as_str());
    assert!(
        (n_changes == 5) && ((last - 5.0).abs() < 1e-9),
        "vsig advances with sim time: {n_changes} change-log entries, current = {last} (expect 5.0)"
    );
}
