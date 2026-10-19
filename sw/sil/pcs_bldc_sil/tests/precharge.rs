mod common;

use common::{spin_up, tap_button};
use pcs_bldc_sil::board::{board, fault_latched, GPIO_LEVEL_HIGH, GPIO_LEVEL_LOW, INPUT_LEVEL_PB10};
use pcs_bldc_sil::{cid, Board, Sil};

/// `prechargeTime_ms` in `app_motorControl_channels.c`.
const PRECHARGE_MS: u32 = 5;
use voyant::Value;

// The drive state as its enumerator name (`Off`, `Precharging`, `Aligning`,
// `Driving`), or the bare value where the DWARF has no name for it.
fn drive_state(sim: &Sil) -> String {
    match sim
        .read(&cid("app_motorControl_data.channels[0].driveState"))
        .ok()
        .flatten()
    {
        Some(Value::Enum(s)) => s,
        other => panic!("driveState mirrored as {other:?}, not an enum"),
    }
}

fn is(sim: &Sil, name: &str, value: u32) -> bool {
    let s = drive_state(sim);
    s.contains(name) || s == format!("<{value}>")
}

// [test->fw~mc_021~1]
#[test]
fn a_re_enable_precharges_then_drives_without_realigning() {
    let Board { mut sim, .. } = board(0.0);
    spin_up(&mut sim);
    assert!(is(&sim, "Driving", 3), "spun up and driving: {}", drive_state(&sim));

    tap_button(&mut sim);
    sim.run_for_ms(50);
    assert!(is(&sim, "Off", 0), "the tap disabled the drive: {}", drive_state(&sim));

    // A lone tap enables once its double-tap window closes; stepping through
    // that window a millisecond at a time catches the states on the way.
    sim.write(&cid(INPUT_LEVEL_PB10), GPIO_LEVEL_LOW).expect("button press");
    sim.run_for_ms(30);
    sim.write(&cid(INPUT_LEVEL_PB10), GPIO_LEVEL_HIGH).expect("button release");
    let mut precharge_ms = 0u32;
    let mut saw_aligning = false;
    let mut driving_at = None;
    for ms in 0..600u32 {
        sim.run_for_ms(1);
        if is(&sim, "Precharging", 1) {
            precharge_ms += 1;
        }
        if is(&sim, "Aligning", 2) {
            saw_aligning = true;
        }
        if is(&sim, "Driving", 3) {
            driving_at = Some(ms);
            break;
        }
    }
    println!("precharge: {precharge_ms} ms of precharge, driving at +{driving_at:?} ms");
    // The dwell is counted in whole 1 ms passes, so it runs the configured time
    // plus up to one pass.
    assert!(
        (PRECHARGE_MS..=PRECHARGE_MS + 1).contains(&precharge_ms),
        "the re-enable charges the bootstraps for the configured time first: {precharge_ms} ms"
    );
    assert!(!saw_aligning, "the alignment, already captured, is not repeated");
    assert!(driving_at.is_some(), "then drives");
    assert!(!fault_latched(&sim), "no fault through the cycle");
}
