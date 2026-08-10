//! North star: the firmware's six-step drive commutating the simulated plant end to end —
//! a button tap arms it, the alignment vector physically swings the rotor and the offset is
//! captured from that physics, then a dial turn spins the shaft under closed-loop commutation.

use pcs_bldc_sil::{
    cid, wire_bridge, wire_current_sense, As5048Model, CurrentSenseModel, MotorModel, MotorParams,
    Sil, SOURCE,
};
use std::f64::consts::{PI, TAU};
use voyant::{vsig_id, Value};

const MOTOR: &str = "motor";
const ENCODER: &str = "as5048_motor";
const DIAL: &str = "dial";
const SENSE: &str = "current_sense";

/// Shaft start, deliberately off the alignment vector so the dwell has to pull it there.
const INITIAL_ANGLE_RAD: f64 = 0.8;
const VBUS_V: f64 = 24.0;

/// `ALIGNMENT_DUTY_CYCLE` / `ALIGNMENT_DWELL_TIMER_MS` in `app_motorControl.c`.
const ALIGN_DUTY: f64 = 0.1;
const ALIGN_DWELL_MS: u64 = 500;

/// `OVERCURRENT_PHASE_TRIP_A` / `OVERCURRENT_BUS_TRIP_A` in `app_motorControl.c`.
const PHASE_TRIP_A: f64 = 2.0;
const BUS_TRIP_A: f64 = 1.5;

/// `APP_MOTORCONTROL_MAX_VELOCITY_RAD_PER_SEC` (200 rpm) and the pole pairs both the
/// firmware channel config and the plant carry.
const MAX_VELOCITY_RAD_S: f64 = 200.0 * TAU / 60.0;
const POLE_PAIRS: f64 = 14.0;

/// `APP_USERCONTROLS_DIAL_ACCUM_RANGE_DEG`: dial travel that maps to full-scale command.
const DIAL_FULL_SCALE_DEG: f64 = 180.0;

// Button: PB10, active LOW (idle HIGH via pull-up). Port B is enum index 1, bit 10.
const INPUT_LEVEL_PB10: &str = "HW_GPIO_data.inputLevel[1][10]";
const GPIO_LEVEL_LOW: u32 = 0;
const GPIO_LEVEL_HIGH: u32 = 1;

// Gate-driver STATUS (reg 0x80 = 128) on the sim I2C register file: BUS_2, sole device.
const I2C_STATUS_REG: &str = "HW_I2C_data.buses[1].devices[0].regMem[128]";
const GATEDRIVER_STATUS_LOCKED: u32 = 0x80;

/// Electrical angle at which the (U high, V low) alignment vector holds the rotor for this
/// trapezoidal shape: where the U and V BEMF shapes meet with U on its falling edge.
const ALIGN_FIELD_RAD_E: f64 = 5.0 * PI / 6.0;
/// One six-step branch advances the applied field by 60 electrical degrees.
const SECTOR_RAD_E: f64 = PI / 3.0;

fn read_bool(sim: &Sil, path: &str) -> bool {
    matches!(sim.read(&cid(path)).ok().flatten(), Some(Value::Bool(true)))
}

fn read_f64(sim: &Sil, path: &str) -> f64 {
    sim.read(&cid(path))
        .ok()
        .flatten()
        .as_ref()
        .and_then(Value::as_f64)
        .unwrap_or(f64::NAN)
}

fn read_u64(sim: &Sil, path: &str) -> u64 {
    sim.read(&cid(path))
        .ok()
        .flatten()
        .as_ref()
        .and_then(Value::as_u64)
        .unwrap_or(0)
}

fn vsig(sim: &Sil, source: &str, local: &str) -> f64 {
    sim.read(&format!("vsig:{source}:{local}"))
        .ok()
        .flatten()
        .as_ref()
        .and_then(Value::as_f64)
        .unwrap_or(f64::NAN)
}

fn port(sim: &Sil, name: &str) -> f64 {
    vsig(sim, SOURCE, name)
}

fn duties(sim: &Sil) -> [f64; 3] {
    [
        port(sim, "PWM_U_duty"),
        port(sim, "PWM_V_duty"),
        port(sim, "PWM_W_duty"),
    ]
}

fn enables(sim: &Sil) -> [f64; 3] {
    [
        port(sim, "PWM_U_enabled"),
        port(sim, "PWM_V_enabled"),
        port(sim, "PWM_W_enabled"),
    ]
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
    let rotor_e = vsig(sim, MOTOR, "angle") * POLE_PAIRS;
    Some((field_e - rotor_e).rem_euclid(TAU).to_degrees())
}

/// Channel-0 gate driver reads operational (mirrors `dev_gateDriver_isOperational`).
fn gate_operational(sim: &Sil) -> bool {
    let c = |f: &str| read_bool(sim, &format!("dev_gateDriver_data.channels[0].{f}"));
    c("configured")
        && c("statusOk")
        && c("locked")
        && !c("resetLatched")
        && !c("vdsProtection")
        && !c("thermalShutdown")
        && !c("vccUndervoltage")
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

fn fault_latched(sim: &Sil) -> bool {
    read_bool(sim, "app_motorControl_data.channels[0].faultLatched")
}

/// One phase current in amps as the firmware itself decoded it on its last 1 ms pass.
fn decoded_phase_a(sim: &Sil, phase: usize) -> f64 {
    read_f64(
        sim,
        &format!("app_motorControl_data.channels[0].phaseCurrent_a[{phase}]"),
    )
}

fn decoded_bus_a(sim: &Sil) -> f64 {
    read_f64(sim, "app_motorControl_data.channels[0].busCurrent")
}

fn peak_decoded_a(sim: &Sil) -> f64 {
    (0..3).fold(0.0_f64, |m, k| m.max(decoded_phase_a(sim, k).abs()))
}

/// The full board world: plant → its encoder → the dial → the sense chain → firmware, every
/// route live, so the physics drives the firmware and the firmware drives the physics.
fn world() -> Sil {
    let mut sim = Sil::new();

    sim.add_member(MotorModel::new(MOTOR, INITIAL_ANGLE_RAD));
    let encoder = sim.add_member(As5048Model::new(ENCODER, 0.0).with_noise(1.52, 0));
    let dial = sim.add_member(As5048Model::new(DIAL, 0.0).with_noise(1.22, 1));
    sim.add_member(CurrentSenseModel::new(SENSE));

    let mut fwm = sim.load_firmware(SOURCE);
    fwm.register_cvar_in_state_table(INPUT_LEVEL_PB10);
    fwm.register_cvar_in_state_table(I2C_STATUS_REG);
    sim.add_member(fwm);

    // The plant's shaft angle is the encoder's input — the test never writes it.
    sim.add_route(
        vsig_id(MOTOR, "angle").expect("valid vsig id"),
        vsig_id(ENCODER, "angle").expect("valid vsig id"),
    )
    .expect("route the shaft angle into the commutation encoder");
    sim.link_duplex("spi:pcs_bldc:AS5048_1", encoder)
        .expect("link the commutation encoder");
    sim.link_duplex("spi:pcs_bldc:AS5048_2", dial)
        .expect("link the dial encoder");
    wire_bridge(&mut sim, SOURCE, MOTOR).expect("wire the bridge into the plant");
    wire_current_sense(&mut sim, MOTOR, SENSE, SOURCE).expect("wire the sense chain");

    sim.write(&format!("vsig:{MOTOR}:v_bus"), VBUS_V)
        .expect("energize the bus");
    sim.write(&cid(I2C_STATUS_REG), GATEDRIVER_STATUS_LOCKED)
        .expect("seed gate STATUS");
    sim.write(&cid(INPUT_LEVEL_PB10), GPIO_LEVEL_HIGH)
        .expect("button idle high");
    sim
}

/// Boot past the gate-driver bring-up, then tap the button and let the double-tap window
/// elapse, so the lone tap toggles run — the arm that starts the alignment dwell.
fn boot_and_arm(sim: &mut Sil) {
    sim.run_for_ms(300);
    assert!(gate_operational(sim), "gate driver operational after boot");
    assert!(!fault_latched(sim), "no fault before drive");
    assert_eq!(max_phase_duty(sim), 0.0, "bridge dark before the arm");

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

    let mut sim = world();
    boot_and_arm(&mut sim);

    // --- The dwell drives the documented alignment pattern -----------------------------
    assert!(mode_is_six_step(&sim), "the arm put the drive in SIX_STEP");
    assert!(
        !read_bool(&sim, "app_motorControl_data.channels[0].isAligned"),
        "still aligning at the top of the dwell"
    );
    let duty_u = port(&sim, "PWM_U_duty");
    assert!(
        (duty_u - ALIGN_DUTY).abs() < 0.02,
        "alignment holds U at ~{ALIGN_DUTY} duty, got {duty_u}"
    );
    assert_eq!(port(&sim, "PWM_U_enabled"), 1.0, "U enabled");
    assert_eq!(port(&sim, "PWM_V_enabled"), 1.0, "V enabled");
    assert_eq!(port(&sim, "PWM_W_enabled"), 0.0, "W held off");
    assert_eq!(port(&sim, "TIM1_MOE"), 1.0, "master output enable on");

    // --- Ride the dwell, sampling the plant's answer every millisecond ------------------
    let mut aligned_at_ms: Option<u64> = None;
    let mut peak_velocity = 0.0_f64;
    let mut peak_current = 0.0_f64;
    let mut peak_bus = 0.0_f64;
    let mut angle = Vec::with_capacity(ALIGN_DWELL_MS as usize + 20);
    let mut decoded_u = Vec::with_capacity(ALIGN_DWELL_MS as usize + 20);
    for ms in 1..=(ALIGN_DWELL_MS + 20) {
        sim.run_for_ms(1);
        peak_velocity = peak_velocity.max(vsig(&sim, MOTOR, "velocity").abs());
        peak_current = peak_current.max(peak_decoded_a(&sim));
        peak_bus = peak_bus.max(decoded_bus_a(&sim).abs());
        angle.push(vsig(&sim, MOTOR, "angle"));
        decoded_u.push(decoded_phase_a(&sim, 0));
        assert!(
            !fault_latched(&sim),
            "no fault latched during the dwell at {ms} ms"
        );
        if aligned_at_ms.is_none() && read_bool(&sim, "app_motorControl_data.channels[0].isAligned")
        {
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
        vsig(&sim, MOTOR, "velocity")
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
    let offset = read_f64(
        &sim,
        "app_motorControl_data.channels[0].alignmentOffset_rad",
    );
    println!(
        "alignment: alignmentOffset {offset:.6} rad vs plant angle at latch \
         {angle_at_latch:.6} rad (encoder decode now {:.6} rad)",
        read_f64(&sim, "IO_AS5048_data.channels[0].angle_rad")
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
        read_u64(&sim, "app_motorControl_data.channels[0].encoderFaultCount") < 5,
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
    /// Friction-limited terminal speed if commutation delivered the full flat-top torque
    /// (ke * 2 * i / B); the floors below are fractions of it.
    const IDEAL_TERMINAL_RAD_S: f64 = 0.9 * VBUS_V / (2.0 * 32.0) * 2.0 * 0.01 / 1.0e-4;
    const VELOCITY_FLOOR_RAD_S: f64 = 0.2 * IDEAL_TERMINAL_RAD_S;
    const REVOLUTION_FLOOR: f64 = 3.0;

    let mut sim = world();
    boot_and_arm(&mut sim);
    sim.run_for_ms(ALIGN_DWELL_MS + 20);
    assert!(
        read_bool(&sim, "app_motorControl_data.channels[0].isAligned"),
        "alignment completed before the dial turn"
    );
    let offset_err_rad = read_f64(
        &sim,
        "app_motorControl_data.channels[0].alignmentOffset_rad",
    ) - alignment_equilibrium_rad(INITIAL_ANGLE_RAD);

    // Wind the dial up in steps: app_userControls accumulates wrapped deltas, so a ramp
    // reads as travel while a single jump past 180 deg would wrap the wrong way.
    let mut deg = 0.0;
    while deg < DIAL_TARGET_DEG {
        deg = (deg + RAMP_STEP_DEG).min(DIAL_TARGET_DEG);
        sim.write(&format!("vsig:{DIAL}:angle[deg]"), deg)
            .expect("turn the dial");
        sim.run_for_ms(RAMP_DWELL_MS);
    }
    let command = DIAL_TARGET_DEG / DIAL_FULL_SCALE_DEG;

    let mut travel = 0.0_f64;
    let mut prev_angle = vsig(&sim, MOTOR, "angle");
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
        read_f64(
            &sim,
            "app_motorControl_data.channels[0].velocitySetpointCurrent_radPerSec"
        ),
        max_phase_duty(&sim)
    );
    println!("spin:    t_ms  velocity   travel  sector   lead  duty   peak|i|    i_bus");

    for ms in 1..=RUN_MS {
        sim.run_for_ms(1);
        assert!(!fault_latched(&sim), "no fault latched at {ms} ms");
        assert!(mode_is_six_step(&sim), "mode stays SIX_STEP at {ms} ms");

        let a = vsig(&sim, MOTOR, "angle");
        let mut step = a - prev_angle;
        if step > PI {
            step -= TAU;
        } else if step < -PI {
            step += TAU;
        }
        travel += step;
        prev_angle = a;

        peak_velocity = peak_velocity.max(vsig(&sim, MOTOR, "velocity").abs());
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
                vsig(&sim, MOTOR, "velocity"),
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

    let final_velocity = vsig(&sim, MOTOR, "velocity");
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
        read_u64(&sim, "app_motorControl_data.channels[0].encoderFaultCount") < 5,
        "encoder fault count stays under the trip limit"
    );
}
