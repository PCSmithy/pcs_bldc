//! North star: the firmware's six-step drive commutating the simulated plant end to end —
//! a button tap arms it, the alignment vector physically swings the rotor and the offset is
//! captured from that physics, then a dial turn spins the shaft under closed-loop commutation.

use pcs_bldc_sil::board::{
    board, decoded_bus_a, decoded_phase_a, fault_latched, gate_operational, port, ALIGN_DUTY,
    ALIGN_DWELL_MS, BUS_TRIP_A, DIAL, GATE_BRINGUP_MS, GPIO_LEVEL_HIGH, GPIO_LEVEL_LOW,
    INPUT_LEVEL_PB10, MOTOR, PHASE_TRIP_A, VBUS_V,
};
use pcs_bldc_sil::{cid, vid, Board, MotorParams, Sil, BRIDGE_PORTS};
use std::f64::consts::{PI, TAU};
use voyant::Value;

/// Shaft start, deliberately off the alignment vector so the dwell has to pull it there.
const INITIAL_ANGLE_RAD: f64 = 0.8;

/// The master output enable, named off the wiring table.
const MOE: &str = BRIDGE_PORTS[6].0;

/// `APP_MOTORCONTROL_MAX_VELOCITY_RAD_PER_SEC` (200 rpm) and the pole pairs both the
/// firmware channel config and the plant carry.
const MAX_VELOCITY_RAD_S: f64 = 200.0 * TAU / 60.0;
const POLE_PAIRS: f64 = 14.0;

/// `APP_USERCONTROLS_DIAL_ACCUM_RANGE_DEG`: dial travel that maps to full-scale command.
const DIAL_FULL_SCALE_DEG: f64 = 180.0;

/// Electrical angle at which the (U high, V low) alignment vector holds the rotor for this
/// trapezoidal shape: where the U and V BEMF shapes meet with U on its falling edge.
const ALIGN_FIELD_RAD_E: f64 = 5.0 * PI / 6.0;
/// One six-step branch advances the applied field by 60 electrical degrees.
const SECTOR_RAD_E: f64 = PI / 3.0;

/// One plant signal (`vsig:motor:<local>`).
fn plant(sim: &Sil, local: &str) -> f64 {
    sim.read_f64(&vid(MOTOR, local))
}

fn duties(sim: &Sil) -> [f64; 3] {
    std::array::from_fn(|k| port(sim, BRIDGE_PORTS[k].0))
}

fn enables(sim: &Sil) -> [f64; 3] {
    std::array::from_fn(|k| port(sim, BRIDGE_PORTS[3 + k].0))
}

fn max_phase_duty(sim: &Sil) -> f64 {
    duties(sim).iter().fold(0.0_f64, |m, d| m.max(*d))
}

/// The commutation branch the bridge is presenting, read back off the PWM ports: the phase
/// carrying duty paired with its enabled return leg, indexed in the firmware's branch order
/// (`app_motorControl.c` six-step table, sector 0 = U high / V low).
fn sector_from_ports(sim: &Sil) -> Option<usize> {
    let d = duties(sim);
    let en = enables(sim);
    let high = (0..3).find(|k| d[*k] > 0.0)?;
    let low = (0..3).find(|k| (*k != high) && (en[*k] != 0.0))?;
    match (high, low) {
        (0, 1) => Some(0),
        (0, 2) => Some(1),
        (1, 2) => Some(2),
        (1, 0) => Some(3),
        (2, 0) => Some(4),
        (2, 1) => Some(5),
        _ => None,
    }
}

/// Electrical degrees the applied field leads the true rotor field by, in [0, 360). The
/// drive motors for a lead under 180 and brakes above it; the firmware aims for 60..120
/// (`app_motorControl.c`, the +30 deg bucket bias on a +90 deg lead).
fn lead_deg(sim: &Sil) -> Option<f64> {
    let field_e = ALIGN_FIELD_RAD_E + (sector_from_ports(sim)? as f64) * SECTOR_RAD_E;
    let rotor_e = plant(sim, "angle") * POLE_PAIRS;
    Some((field_e - rotor_e).rem_euclid(TAU).to_degrees())
}

fn mode_is_six_step(sim: &Sil) -> bool {
    match sim
        .read(&cid("app_motorControl_data.channels[0].modeCurrent"))
        .ok()
        .flatten()
    {
        Some(Value::Enum(s)) => s.contains("SIX_STEP") || s == "<1>",
        other => panic!("modeCurrent mirrored as {other:?}, not an enum"),
    }
}

fn is_aligned(sim: &Sil) -> bool {
    sim.read_bool(&cid("app_motorControl_data.channels[0].isAligned"))
}

fn peak_decoded_a(sim: &Sil) -> f64 {
    (0..3).fold(0.0_f64, |m, k| m.max(decoded_phase_a(sim, k).abs()))
}

/// Boot past the gate-driver bring-up, then tap the button and let the double-tap window
/// elapse, so the lone tap toggles run — the arm that starts the alignment dwell.
fn boot_and_arm(sim: &mut Sil) {
    sim.run_for_ms(GATE_BRINGUP_MS);
    assert!(gate_operational(sim), "gate driver operational after boot");
    assert!(!fault_latched(sim), "no fault before drive");
    assert_eq!(max_phase_duty(sim), 0.0, "bridge dark before the arm");

    // Press (LOW) then release (HIGH), each held past the 20 ms debounce so dev_switch
    // latches ACTIVE then INACTIVE — a tap.
    sim.write(&cid(INPUT_LEVEL_PB10), GPIO_LEVEL_LOW)
        .expect("button press");
    sim.run_for_ms(30);
    sim.write(&cid(INPUT_LEVEL_PB10), GPIO_LEVEL_HIGH)
        .expect("button release");
    sim.run_for_ms(30);
    sim.run_for_ms(320); // APP_USERCONTROLS_DOUBLE_TAP_WINDOW_MS plus slack
}

/// Mechanical angle the alignment vector pulls to: the electrical equilibrium lifted to the
/// revolution nearest the shaft's start, since the basin around it runs +/- pi electrical.
fn alignment_equilibrium_rad(start_rad: f64) -> f64 {
    let start_e = start_rad * POLE_PAIRS;
    let k = ((start_e - ALIGN_FIELD_RAD_E) / TAU).round();
    (ALIGN_FIELD_RAD_E + (k * TAU)) / POLE_PAIRS
}

#[test]
fn alignment_pulls_rotor_and_captures_offset() {
    // Alignment is a torsional spring: 0.1 duty across U-V drives 0.1*Vbus/2R through the
    // windings and the rotor is pulled to where the U and V BEMF shapes meet.
    const CURRENT_BAND_A: f64 = 0.015;
    const VELOCITY_FLOOR_RAD_S: f64 = 0.05;
    const OFFSET_MATCH_RAD: f64 = 0.01;

    let params = MotorParams::default();
    let expected_current_a = (ALIGN_DUTY * VBUS_V) / (2.0 * params.r_ohm);
    let equilibrium = alignment_equilibrium_rad(INITIAL_ANGLE_RAD);

    let Board { mut sim, .. } = board(INITIAL_ANGLE_RAD);
    boot_and_arm(&mut sim);

    // --- The dwell drives the documented alignment pattern -----------------------------
    assert!(mode_is_six_step(&sim), "the arm put the drive in SIX_STEP");
    assert!(!is_aligned(&sim), "still aligning at the top of the dwell");
    let duty_u = port(&sim, "PWM_U_duty");
    assert!(
        (duty_u - ALIGN_DUTY).abs() < 0.02,
        "alignment holds U at ~{ALIGN_DUTY} duty, got {duty_u}"
    );
    assert_eq!(port(&sim, "PWM_U_enabled"), 1.0, "U enabled");
    assert_eq!(port(&sim, "PWM_V_enabled"), 1.0, "V enabled");
    assert_eq!(port(&sim, "PWM_W_enabled"), 0.0, "W held off");
    assert_eq!(port(&sim, MOE), 1.0, "master output enable on");

    // --- Ride the dwell, sampling the plant's answer every millisecond ------------------
    let mut aligned_at_ms: Option<u64> = None;
    let mut peak_velocity = 0.0_f64;
    let mut peak_current = 0.0_f64;
    let mut peak_bus = 0.0_f64;
    let mut angle = Vec::with_capacity(ALIGN_DWELL_MS as usize + 20);
    let mut decoded_u = Vec::with_capacity(ALIGN_DWELL_MS as usize + 20);
    for ms in 1..=(ALIGN_DWELL_MS + 20) {
        sim.run_for_ms(1);
        peak_velocity = peak_velocity.max(plant(&sim, "velocity").abs());
        peak_current = peak_current.max(peak_decoded_a(&sim));
        peak_bus = peak_bus.max(decoded_bus_a(&sim).abs());
        angle.push(plant(&sim, "angle"));
        decoded_u.push(decoded_phase_a(&sim, 0));
        assert!(
            !fault_latched(&sim),
            "no fault latched during the dwell at {ms} ms"
        );
        if aligned_at_ms.is_none() && is_aligned(&sim) {
            aligned_at_ms = Some(ms);
        }
    }
    let aligned_at_ms = aligned_at_ms.expect("isAligned latched within the dwell");
    let latch_idx = aligned_at_ms as usize - 1;

    // The dwell pattern is dropped the instant the offset is captured, so the current is
    // averaged over a window strictly inside it.
    let window = &decoded_u[200..latch_idx.min(400)];
    let settled_current = window.iter().sum::<f64>() / (window.len() as f64);
    let angle_at_latch = angle[latch_idx];
    let offset_err_rad = angle_at_latch - equilibrium;

    println!(
        "alignment: i_u expected {expected_current_a:.6} A, decoded mean over the dwell \
         {settled_current:.6} A (band +/-{CURRENT_BAND_A} A); peak |decoded phase| \
         {peak_current:.6} A of a {PHASE_TRIP_A} A trip, peak |decoded bus| {peak_bus:.6} A \
         of {BUS_TRIP_A} A"
    );
    println!(
        "alignment: start {INITIAL_ANGLE_RAD:.6} rad, expected equilibrium \
         {equilibrium:.6} rad, angle at latch {angle_at_latch:.6} rad -> error \
         {offset_err_rad:+.6} rad ({:+.1} deg electrical); peak |velocity| \
         {peak_velocity:.4} rad/s, velocity at latch {:+.4} rad/s, isAligned at \
         {aligned_at_ms} ms",
        (offset_err_rad * POLE_PAIRS).to_degrees(),
        plant(&sim, "velocity")
    );
    println!(
        "alignment: angle trace @0/100/200/300/400/500 ms = {:.6} {:.6} {:.6} {:.6} {:.6} {:.6} \
         (min {:.6}, max {:.6})",
        angle[0],
        angle[99],
        angle[199],
        angle[299],
        angle[399],
        angle[499],
        angle.iter().cloned().fold(f64::INFINITY, f64::min),
        angle.iter().cloned().fold(f64::NEG_INFINITY, f64::max)
    );

    // --- Real current flows, positive on U ---------------------------------------------
    assert!(
        (settled_current - expected_current_a).abs() < CURRENT_BAND_A,
        "phase U carries the alignment current {expected_current_a:.4} A within \
         {CURRENT_BAND_A} A: {settled_current:.6} A"
    );
    assert!(
        settled_current > 0.0,
        "the alignment current is positive on U: {settled_current:.6} A"
    );
    assert!(
        peak_current < PHASE_TRIP_A,
        "phase currents stay under the {PHASE_TRIP_A} A trip: {peak_current:.4} A"
    );
    assert!(
        peak_bus < BUS_TRIP_A,
        "bus current stays under the {BUS_TRIP_A} A trip: {peak_bus:.4} A"
    );

    // --- The rotor physically swings to the alignment vector ---------------------------
    // The plant is very underdamped at the placeholder parameters (zeta ~ 0.05 for
    // J = 1e-4, B = 1e-4 against the ~1e-2 Nm/rad alignment spring), so the 500 ms dwell
    // buys most of one swing, not a settle: assert the traverse and the basin, not rest.
    assert!(
        peak_velocity > VELOCITY_FLOOR_RAD_S,
        "the alignment vector swings the rotor: peak |velocity| {peak_velocity:.6} rad/s"
    );
    let min_angle = angle.iter().cloned().fold(f64::INFINITY, f64::min);
    assert!(
        (min_angle < equilibrium) && (angle[0] > equilibrium),
        "the shaft crosses the equilibrium {equilibrium:.6} rad on its way in: swept \
         {:.6} -> {min_angle:.6} rad",
        angle[0]
    );
    assert!(
        offset_err_rad.abs() < (PI / POLE_PAIRS),
        "the shaft is inside the alignment vector's basin (+/- pi electrical) when the \
         offset is captured: {offset_err_rad:+.6} rad"
    );

    // --- The offset is the encoder's decode of the shaft the dwell left behind ----------
    let offset = sim.read_f64(&cid(
        "app_motorControl_data.channels[0].alignmentOffset_rad",
    ));
    println!(
        "alignment: alignmentOffset {offset:.6} rad vs plant angle at latch \
         {angle_at_latch:.6} rad (encoder decode now {:.6} rad)",
        sim.read_f64(&cid("IO_AS5048_data.channels[0].angle_rad"))
    );
    assert!(
        (offset - angle_at_latch).abs() < OFFSET_MATCH_RAD,
        "the captured offset is the encoder's decode of the physical shaft angle: \
         {offset:.6} rad vs {angle_at_latch:.6} rad"
    );
    assert!(
        !fault_latched(&sim),
        "no fault latched across the whole alignment"
    );
    assert!(
        sim.read_u64(&cid("app_motorControl_data.channels[0].encoderFaultCount")) < 5,
        "encoder fault count stays under the trip limit"
    );
}

#[test]
fn dial_demand_spins_rotor_closed_loop() {
    // Full-scale dial: duty clamps at APP_MOTORCONTROL_MAX_DUTY_01 = 0.9, so the winding
    // carries 0.9*Vbus/2R = 0.34 A — an order under the 2.0 A phase trip.
    const DIAL_TARGET_DEG: f64 = 180.0;
    const RAMP_STEP_DEG: f64 = 10.0;
    const RAMP_DWELL_MS: u64 = 20;
    const RUN_MS: u64 = 3000;
    const IDLE_MS: u64 = 50;
    /// Friction-limited terminal speed if commutation delivered the full flat-top torque
    /// (ke * 2 * i / B); the floors below are fractions of it.
    const IDEAL_TERMINAL_RAD_S: f64 = 0.9 * VBUS_V / (2.0 * 32.0) * 2.0 * 0.01 / 1.0e-4;
    const VELOCITY_FLOOR_RAD_S: f64 = 0.2 * IDEAL_TERMINAL_RAD_S;
    const REVOLUTION_FLOOR: f64 = 3.0;

    let Board { mut sim, .. } = board(INITIAL_ANGLE_RAD);
    boot_and_arm(&mut sim);
    sim.run_for_ms(ALIGN_DWELL_MS + 20);
    assert!(is_aligned(&sim), "alignment completed before the dial turn");
    let offset_err_rad = sim.read_f64(&cid(
        "app_motorControl_data.channels[0].alignmentOffset_rad",
    )) - alignment_equilibrium_rad(INITIAL_ANGLE_RAD);

    // --- Aligned and armed at zero demand: the phases idle, the bridge does not flap ----
    for _ in 0..IDLE_MS {
        sim.run_for_ms(1);
        assert_eq!(
            port(&sim, MOE),
            1.0,
            "MOE holds asserted across the zero-demand window"
        );
        assert!(
            max_phase_duty(&sim) < 0.02,
            "phases stay idle across the zero-demand window"
        );
    }
    assert!(
        is_aligned(&sim),
        "alignment is retained across the zero-demand window"
    );

    // --- The first dial turn commutates at once on the stored offset --------------------
    // +90 deg -> command +0.5 -> half-scale duty, within a few control cycles and with no
    // second dwell. The accumulator sums wrapped deltas, so this excursion nets out of the
    // ramp below (which ends at the same 180 deg of travel).
    sim.write(&vid(DIAL, "angle[deg]"), 90.0)
        .expect("turn the dial");
    sim.run_for_ms(5);
    assert!(
        is_aligned(&sim),
        "no second dwell: alignment stays latched through the first dial turn"
    );
    assert_eq!(port(&sim, MOE), 1.0, "MOE still asserted while commutating");
    let driven = max_phase_duty(&sim);
    assert!(
        driven > 0.2,
        "commutation drive appears at once on the first dial turn, got {driven}"
    );

    // Wind the dial up in steps: app_userControls accumulates wrapped deltas, so a ramp
    // reads as travel while a single jump past 180 deg would wrap the wrong way.
    let mut deg = 0.0;
    while deg < DIAL_TARGET_DEG {
        deg = (deg + RAMP_STEP_DEG).min(DIAL_TARGET_DEG);
        sim.write(&vid(DIAL, "angle[deg]"), deg)
            .expect("turn the dial");
        sim.run_for_ms(RAMP_DWELL_MS);
    }
    let command = DIAL_TARGET_DEG / DIAL_FULL_SCALE_DEG;

    let mut travel = 0.0_f64;
    let mut prev_angle = plant(&sim, "angle");
    let mut peak_velocity = 0.0_f64;
    let mut peak_current = 0.0_f64;
    let mut peak_bus = 0.0_f64;
    let mut visited = [false; 6];
    let mut lead_min = f64::INFINITY;
    let mut lead_max = f64::NEG_INFINITY;
    let mut lead_sum = 0.0_f64;
    let mut lead_n = 0_u64;

    println!(
        "spin: alignment offset error {offset_err_rad:+.6} rad ({:+.1} deg electrical); dial \
         {DIAL_TARGET_DEG} deg -> command {command:.3} -> setpoint {:.4} rad/s (of \
         {MAX_VELOCITY_RAD_S:.4} max), duty {:.3}",
        (offset_err_rad * POLE_PAIRS).to_degrees(),
        sim.read_f64(&cid(
            "app_motorControl_data.channels[0].velocitySetpointCurrent_radPerSec"
        )),
        max_phase_duty(&sim)
    );
    println!("spin:    t_ms  velocity   travel  sector   lead  duty   peak|i|    i_bus");

    for ms in 1..=RUN_MS {
        sim.run_for_ms(1);
        assert!(!fault_latched(&sim), "no fault latched at {ms} ms");
        assert!(mode_is_six_step(&sim), "mode stays SIX_STEP at {ms} ms");

        let a = plant(&sim, "angle");
        let mut step = a - prev_angle;
        if step > PI {
            step -= TAU;
        } else if step < -PI {
            step += TAU;
        }
        travel += step;
        prev_angle = a;

        peak_velocity = peak_velocity.max(plant(&sim, "velocity").abs());
        peak_current = peak_current.max(peak_decoded_a(&sim));
        peak_bus = peak_bus.max(decoded_bus_a(&sim).abs());
        if let Some(s) = sector_from_ports(&sim) {
            visited[s] = true;
        }
        if let Some(l) = lead_deg(&sim) {
            lead_min = lead_min.min(l);
            lead_max = lead_max.max(l);
            lead_sum += l;
            lead_n += 1;
        }

        if ms % 100 == 0 {
            println!(
                "spin:  {ms:>6}  {:+8.3}  {travel:+7.2}  {:>6}  {:>5.0}  {:.3}   {:.4}   {:.4}",
                plant(&sim, "velocity"),
                sector_from_ports(&sim)
                    .map(|s| s.to_string())
                    .unwrap_or_else(|| "-".into()),
                lead_deg(&sim).unwrap_or(f64::NAN),
                max_phase_duty(&sim),
                peak_decoded_a(&sim),
                decoded_bus_a(&sim)
            );
        }
    }

    let final_velocity = plant(&sim, "velocity");
    let revolutions = travel / TAU;
    println!(
        "spin: final velocity {final_velocity:+.3} rad/s (peak {peak_velocity:.3}, floor \
         {VELOCITY_FLOOR_RAD_S:.3}, ideal terminal {IDEAL_TERMINAL_RAD_S:.3}), travel \
         {travel:+.3} rad = {revolutions:+.2} mech rev ({:.1} electrical rev), sectors \
         {visited:?}, lead {lead_min:.0}..{lead_max:.0} deg mean {:.1} (design band \
         60..120), peak |i| {peak_current:.4} A, peak |i_bus| {peak_bus:.4} A",
        revolutions * POLE_PAIRS,
        lead_sum / (lead_n as f64)
    );

    assert!(
        visited.iter().all(|v| *v),
        "the commutation walks all six sectors: {visited:?}"
    );
    assert!(
        final_velocity > VELOCITY_FLOOR_RAD_S,
        "the rotor spins forward under a forward demand: {final_velocity:.3} rad/s against \
         a {VELOCITY_FLOOR_RAD_S:.3} rad/s floor"
    );
    assert!(
        revolutions > REVOLUTION_FLOOR,
        "the shaft turns through multiple revolutions: {revolutions:.2}"
    );
    assert!(
        peak_current < PHASE_TRIP_A,
        "phase currents stay under the {PHASE_TRIP_A} A trip: {peak_current:.4} A"
    );
    assert!(
        peak_bus < BUS_TRIP_A,
        "bus current stays under the {BUS_TRIP_A} A trip: {peak_bus:.4} A"
    );
    assert!(
        sim.read_u64(&cid("app_motorControl_data.channels[0].encoderFaultCount")) < 5,
        "encoder fault count stays under the trip limit"
    );
}
