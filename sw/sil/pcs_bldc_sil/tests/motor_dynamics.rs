//! Physics validation of the motor + inverter plant in a model-only world: dark bridge,
//! locked-rotor divider, alignment, demagnetization, KCL through commutation, coast time
//! constants, common-mode rejection, and diode rectification on a collapsed bus.

use pcs_bldc_sil::board::{MOTOR, VBUS_V};
use pcs_bldc_sil::{vid, MotorModel, MotorParams, Sil, TICK_US};
use std::f64::consts::{PI, TAU};
use voyant::StateTableConfig;

const TICK_S: f64 = (TICK_US as f64) * 1e-6;

const DUTY_PORTS: [&str; 3] = ["duty_u", "duty_v", "duty_w"];
const ENABLE_PORTS: [&str; 3] = ["enable_u", "enable_v", "enable_w"];
const MOE_PORT: &str = "moe";

/// A model-only world: the motor alone with its bus energized, and exact change
/// detection so every milliamp of model state reaches the table — the default 1e-3
/// epsilon is a deadband wider than several of the quantities under test. Nothing
/// routes into the motor's inputs, so the tests drive them by direct write.
fn world(params: MotorParams, initial_angle_rad: f64, vbus: f64) -> Sil {
    let config = StateTableConfig {
        epsilon: 0.0,
        ..StateTableConfig::default()
    };
    let mut sim = Sil::with_config(config);
    sim.add_member(MotorModel::new(MOTOR, initial_angle_rad).with_params(params));
    sim.write(&vid(MOTOR, "v_bus"), vbus).expect("write vbus");
    sim
}

#[derive(Clone, Copy, Debug)]
struct Sample {
    angle: f64,
    velocity: f64,
    current: [f64; 3],
    torque: f64,
}

impl Sample {
    fn kcl_residual(&self) -> f64 {
        self.current.iter().sum::<f64>().abs()
    }

    fn peak_current(&self) -> f64 {
        self.current.iter().fold(0.0f64, |m, c| m.max(c.abs()))
    }
}

fn sample(eng: &Sil) -> Sample {
    Sample {
        angle: eng.read_f64(&vid(MOTOR, "angle")),
        velocity: eng.read_f64(&vid(MOTOR, "velocity")),
        current: [
            eng.read_f64(&vid(MOTOR, "phase_current_u")),
            eng.read_f64(&vid(MOTOR, "phase_current_v")),
            eng.read_f64(&vid(MOTOR, "phase_current_w")),
        ],
        torque: eng.read_f64(&vid(MOTOR, "torque")),
    }
}

/// Publish one bridge command: per-phase duty + enable and the master output enable.
fn drive(eng: &mut Sil, duty: [f64; 3], enabled: [bool; 3], moe: bool) {
    for (k, (d, en)) in duty.iter().zip(enabled.iter()).enumerate() {
        eng.write(&vid(MOTOR, DUTY_PORTS[k]), *d)
            .expect("write duty");
        eng.write(&vid(MOTOR, ENABLE_PORTS[k]), f64::from(u8::from(*en)))
            .expect("write enable");
    }
    eng.write(&vid(MOTOR, MOE_PORT), f64::from(u8::from(moe)))
        .expect("write MOE");
}

fn step_n(eng: &mut Sil, n: usize) {
    for _ in 0..n {
        eng.step().expect("engine step");
    }
}

/// (high-side phase, low-side phase) per 60-degree electrical sector, sector 0 starting at
/// theta_e = pi/6 where the U and V shapes sit on opposite flat tops.
const SECTOR_DRIVE: [(usize, usize); 6] = [(0, 1), (0, 2), (1, 2), (1, 0), (2, 0), (2, 1)];

fn sector_of(theta_e: f64) -> usize {
    let s = (theta_e - (PI / 6.0)).rem_euclid(TAU) / (PI / 3.0);
    (s as usize).min(5)
}

/// Commutate for the shaft angle just observed: the two flat-top phases carry current in
/// the sign of their BEMF shape and the ramping phase floats. Returns the sector driven.
fn commutate(eng: &mut Sil, angle_rad: f64, pole_pairs: u8, duty: f64) -> usize {
    let sector = sector_of(angle_rad * f64::from(pole_pairs));
    let (high, low) = SECTOR_DRIVE[sector];
    let mut d = [0.0; 3];
    let mut en = [false; 3];
    d[high] = duty;
    en[high] = true;
    en[low] = true;
    drive(eng, d, en, true);
    sector
}

/// Locked-rotor parameters: inertia and friction large enough that the shaft cannot move,
/// so the electrical side is exercised at zero BEMF.
fn locked_params(r_ohm: f64, l_h: f64) -> MotorParams {
    MotorParams {
        r_ohm,
        l_h,
        ke_v_per_mech_rad_s: 0.05,
        pole_pairs: 7,
        j_kg_m2: 1.0e3,
        b_nm_per_rad_s: 1.0e2,
        t_c_nm: 0.0,
        v_d: 0.8,
    }
}

/// Free-running parameters for the six-step tests. The tests derive their time
/// constants and horizons from these values (tau_mech = J/B, engagement bands),
/// so retuning a parameter retunes the expectations with it.
fn spin_params() -> MotorParams {
    MotorParams {
        r_ohm: 8.0,
        l_h: 1.0e-3,
        ke_v_per_mech_rad_s: 0.05,
        pole_pairs: 2,
        j_kg_m2: 2.0e-4,
        b_nm_per_rad_s: 1.0e-4,
        t_c_nm: 0.0,
        v_d: 0.8,
    }
}

const SPIN_POLE_PAIRS: u8 = 2;
const SPIN_DUTY: f64 = 0.5;

fn tau_mech_s(params: &MotorParams) -> f64 {
    params.j_kg_m2 / params.b_nm_per_rad_s
}

#[test]
fn dark_bridge_stays_dark() {
    // No terminal drive means no winding current: torque, motion, and every phase current
    // hold at exactly zero however long the bus stays energized.
    const ANGLE_RAD: f64 = 0.75;
    const TICKS: usize = 50;

    let mut eng = world(MotorParams::default(), ANGLE_RAD, VBUS_V);
    step_n(&mut eng, TICKS);
    let s = sample(&eng);
    println!("dark_bridge_stays_dark: {s:?}");

    assert_eq!(
        s.current,
        [0.0, 0.0, 0.0],
        "an unwritten bridge draws no phase current: {:?}",
        s.current
    );
    assert_eq!(s.torque, 0.0, "no current means no torque: {}", s.torque);
    assert_eq!(
        s.velocity, 0.0,
        "no torque means the rotor never turns: {}",
        s.velocity
    );
    assert_eq!(
        s.angle, ANGLE_RAD,
        "the shaft holds its initial angle: {}",
        s.angle
    );
    let finite = s.angle.is_finite()
        && s.velocity.is_finite()
        && s.torque.is_finite()
        && s.current.iter().all(|c| c.is_finite());
    assert!(finite, "a dark bridge publishes finite state: {s:?}");
}

#[test]
fn locked_rotor_current_matches_analytic() {
    // A locked shaft produces no BEMF, so the two driven windings are a plain resistive
    // divider: the duty difference across 2R sets the current and the float leg carries none.
    const R_OHM: f64 = 8.0;
    const DUTY_HIGH: f64 = 0.75;
    const DUTY_LOW: f64 = 0.25;
    const TICKS: usize = 50; // 50 ms against tau_elec = 125 us

    let mut eng = world(locked_params(R_OHM, 1.0e-3), 0.4, VBUS_V);
    drive(
        &mut eng,
        [DUTY_HIGH, DUTY_LOW, 0.0],
        [true, true, false],
        true,
    );
    step_n(&mut eng, TICKS);
    let s = sample(&eng);

    let expected = (DUTY_HIGH - DUTY_LOW) * VBUS_V / (2.0 * R_OHM);
    let error = (s.current[0] - expected).abs() / expected;
    println!(
        "locked_rotor_current_matches_analytic: i_u = {:.9} A (expected {expected:.9}, error {:.3}%), \
         i_v = {:.9}, i_w = {:.9}, sum = {:.3e}, velocity = {:.3e} rad/s",
        s.current[0],
        error * 100.0,
        s.current[1],
        s.current[2],
        s.current.iter().sum::<f64>(),
        s.velocity
    );

    assert!(
        error < 0.01,
        "the settled current is (d_high - d_low) * Vbus / 2R = {expected:.6} A within 1%: \
         measured {:.6} A",
        s.current[0]
    );
    assert!(
        (s.current[1] + s.current[0]).abs() < 1.0e-5,
        "the two driven phases carry equal and opposite current: i_u = {:.9}, i_v = {:.9}",
        s.current[0],
        s.current[1]
    );
    assert_eq!(
        s.current[2], 0.0,
        "the disabled leg floats and carries nothing: i_w = {}",
        s.current[2]
    );
    assert!(
        s.kcl_residual() < 1.0e-5,
        "the phase currents sum to zero: residual = {:.3e} A",
        s.kcl_residual()
    );
}

#[test]
fn alignment_torque_pulls_to_equilibrium() {
    // A held U-high / V-low vector is a torsional spring: from an offset angle the rotor
    // swings to where the two energized BEMF shapes match (theta_e = 5pi/6) and stops.
    const POLE_PAIRS: u8 = 7;
    const HALF_SETTLE_TICKS: usize = 200; // 200 ms against a ~26 ms dominant pole
    const REST_RAD_S: f64 = 1.0e-3;

    let params = MotorParams {
        r_ohm: 4.0,
        l_h: 1.0e-3,
        ke_v_per_mech_rad_s: 0.05,
        pole_pairs: POLE_PAIRS,
        j_kg_m2: 1.0e-4,
        b_nm_per_rad_s: 0.03,
        t_c_nm: 0.0,
        v_d: 0.8,
    };
    let equilibrium = (5.0 * PI / 6.0) / f64::from(POLE_PAIRS);
    let start = (PI / 2.0) / f64::from(POLE_PAIRS);

    let mut eng = world(params, start, VBUS_V);
    drive(&mut eng, [0.75, 0.25, 0.0], [true, true, false], true);
    step_n(&mut eng, 2);
    let early = sample(&eng);
    step_n(&mut eng, HALF_SETTLE_TICKS);
    let half = sample(&eng);
    step_n(&mut eng, HALF_SETTLE_TICKS);
    let settled = sample(&eng);

    println!(
        "alignment_torque_pulls_to_equilibrium: start = {start:.6} rad, equilibrium = {equilibrium:.6} rad, \
         early torque = {:.6} Nm at {:.6} rad; at {HALF_SETTLE_TICKS} ms offset = {:.3e} rad, \
         velocity = {:.3e} rad/s; at {} ms offset = {:.3e} rad, velocity = {:.3e} rad/s, torque = {:.3e} Nm",
        early.torque,
        early.angle,
        half.angle - equilibrium,
        half.velocity,
        2 * HALF_SETTLE_TICKS,
        settled.angle - equilibrium,
        settled.velocity,
        settled.torque
    );

    assert!(
        early.torque > 0.05,
        "an offset rotor sees restoring torque toward the energized vector: {:.6} Nm",
        early.torque
    );
    assert!(
        (early.angle > start) && (early.angle < equilibrium),
        "the shaft moves toward equilibrium: {:.6} rad is not between {start:.6} and {equilibrium:.6}",
        early.angle
    );
    assert!(
        (settled.angle - equilibrium).abs() < 0.02,
        "the shaft converges on theta_e = 5pi/6: {:.6} rad vs {equilibrium:.6} rad",
        settled.angle
    );
    assert!(
        (settled.angle - equilibrium).abs() <= (half.angle - equilibrium).abs(),
        "the approach is monotone, not an oscillation: {:.3e} rad at {} ms after {:.3e} rad at \
         {HALF_SETTLE_TICKS} ms",
        settled.angle - equilibrium,
        2 * HALF_SETTLE_TICKS,
        half.angle - equilibrium
    );
    assert!(
        settled.velocity.abs() < REST_RAD_S,
        "the shaft comes to rest: {:.6} rad/s",
        settled.velocity
    );
}

/// One demagnetization run: settle a locked-rotor current at the given duties, then drop
/// both enables and watch the freewheel path unwind it.
fn demag_case(label: &str, duty_u: f64, duty_v: f64) {
    const SETTLE_TICKS: usize = 200; // 200 ms against tau_elec = 20 ms
    const DECAY_TICKS: usize = 25;
    const HOLD_TICKS: usize = 25;

    let mut eng = world(locked_params(1.0, 20.0e-3), 0.4, VBUS_V);
    drive(&mut eng, [duty_u, duty_v, 0.0], [true, true, false], true);
    step_n(&mut eng, SETTLE_TICKS);
    let established = sample(&eng);
    assert!(
        established.current[0].abs() > 1.0,
        "{label}: the drive establishes winding current first: i_u = {:.6} A",
        established.current[0]
    );

    // Both legs off with the master enable still asserted: the body diodes clamp each
    // winding to a rail and force its current to zero.
    drive(&mut eng, [duty_u, duty_v, 0.0], [false, false, false], true);
    let mut prev = [
        established.current[0].abs(),
        established.current[1].abs(),
        established.current[2].abs(),
    ];
    let mut trace = Vec::with_capacity(DECAY_TICKS);
    for tick in 0..DECAY_TICKS {
        eng.step().expect("engine step");
        let s = sample(&eng);
        for (c, p) in s.current.iter().zip(prev.iter_mut()) {
            assert!(
                c.abs() <= *p,
                "{label}: freewheel current only decays in magnitude; tick {tick} went \
                 {p:.9} -> {:.9} A",
                c.abs()
            );
            *p = c.abs();
        }
        trace.push(s);
    }

    let zero_at = trace
        .iter()
        .position(|s| s.current == [0.0, 0.0, 0.0])
        .unwrap_or_else(|| {
            panic!(
                "{label}: freewheel reaches exactly zero within {DECAY_TICKS} ms; \
                 last sample {:?}",
                trace[DECAY_TICKS - 1].current
            )
        });
    println!(
        "{label}: established i = [{:.6}, {:.6}, {:.6}] A, exact zero at tick {zero_at} of {DECAY_TICKS}",
        established.current[0], established.current[1], established.current[2]
    );

    for tick in 0..HOLD_TICKS {
        eng.step().expect("engine step");
        let s = sample(&eng);
        assert_eq!(
            s.current,
            [0.0, 0.0, 0.0],
            "{label}: a demagnetized bridge stays dark; tick {tick} reads {:?}",
            s.current
        );
    }
}

#[test]
fn demag_decays_to_exact_zero_both_rails() {
    // Killing both enables leaves the winding energy to the body diodes: the current decays
    // monotonically to exactly zero and stays there, whichever rail clamps it.
    demag_case("demag_positive_i_u", 0.75, 0.25);
    demag_case("demag_negative_i_u", 0.25, 0.75);
}

#[test]
fn kcl_holds_through_commutation() {
    // Commutation opens and clamps legs mid-tick, injecting sub-step-sized KCL residue that
    // relaxes at tau_elec; the three phase currents still sum to zero at every tick boundary.
    const TICKS: usize = 2000;
    const TOLERANCE_A: f64 = 1.0e-3;

    let mut eng = world(spin_params(), PI / 8.0, VBUS_V);
    let mut angle = PI / 8.0;
    let mut worst = 0.0f64;
    let mut worst_tick = 0usize;
    for tick in 0..TICKS {
        commutate(&mut eng, angle, SPIN_POLE_PAIRS, SPIN_DUTY);
        eng.step().expect("engine step");
        let s = sample(&eng);
        angle = s.angle;
        if s.kcl_residual() > worst {
            worst = s.kcl_residual();
            worst_tick = tick;
        }
    }
    println!(
        "kcl_holds_through_commutation: worst |sum i| = {worst:.3e} A at tick {worst_tick} \
         of {TICKS} (tolerance {TOLERANCE_A:.0e}), final velocity = {:.3} rad/s",
        sample(&eng).velocity
    );

    assert!(
        worst < TOLERANCE_A,
        "the phase currents sum to zero through every commutation: worst {worst:.3e} A \
         at tick {worst_tick}"
    );
}

#[test]
fn six_step_spins_then_coasts_at_tau_mech() {
    // Six-step drive sustains rotation through all six sectors; cutting the master enable
    // below the diode window leaves only viscous friction, so omega decays as exp(-t/(J/B)).
    const SPIN_TICKS: usize = 800;
    const SPEED_FLOOR_RAD_S: f64 = 60.0;

    let params = spin_params();
    let tau = tau_mech_s(&params);
    // Coast 1.25 tau so the fit spans a deep stretch of the exponential (capped so a
    // very heavy plant still finishes quickly; the fit only needs measurable decay).
    let coast_ticks = ((1.25 * tau / TICK_S).ceil() as usize).clamp(50, 5000);

    let mut eng = world(params, PI / 8.0, VBUS_V);
    let mut angle = PI / 8.0;
    let mut visited = [false; 6];
    for _ in 0..SPIN_TICKS {
        visited[commutate(&mut eng, angle, SPIN_POLE_PAIRS, SPIN_DUTY)] = true;
        eng.step().expect("engine step");
        angle = sample(&eng).angle;
    }
    let spinning = sample(&eng);
    let line_to_line_bemf = 2.0 * params.ke_v_per_mech_rad_s * spinning.velocity;

    // Coast: master output enable off, every leg released.
    drive(&mut eng, [0.0; 3], [false; 3], false);
    let mut omega = Vec::with_capacity(coast_ticks);
    for _ in 0..coast_ticks {
        eng.step().expect("engine step");
        omega.push(sample(&eng).velocity);
    }

    let w0 = omega[0];
    let errors: Vec<(usize, f64, f64, f64)> = [0.2, 0.4, 0.6, 0.8, 1.0]
        .iter()
        .map(|f| {
            let k = ((((coast_ticks - 1) as f64) * f) as usize).max(1);
            let predicted = w0 * (-((k as f64) * TICK_S) / tau).exp();
            (
                k,
                omega[k],
                predicted,
                (omega[k] - predicted).abs() / predicted,
            )
        })
        .collect();
    println!(
        "six_step_spins_then_coasts_at_tau_mech: spun to {:.3} rad/s (line-to-line BEMF \
         {line_to_line_bemf:.3} V of a {:.1} V window), sectors {visited:?}",
        spinning.velocity,
        VBUS_V + 2.0 * params.v_d
    );
    for (k, measured, predicted, rel) in &errors {
        println!(
            "  coast t = {} ms: omega = {measured:.4} rad/s vs {predicted:.4} predicted \
             ({:.3}% error)",
            *k + 1,
            rel * 100.0
        );
    }

    assert!(
        spinning.velocity > SPEED_FLOOR_RAD_S,
        "six-step drive sustains rotation: {:.3} rad/s",
        spinning.velocity
    );
    assert!(
        visited.iter().all(|v| *v),
        "the rotor passes through all six commutation sectors: {visited:?}"
    );
    assert!(
        line_to_line_bemf < (VBUS_V - 2.0 * params.v_d),
        "the coast speed keeps the BEMF inside the diode window: {line_to_line_bemf:.3} V"
    );
    for (k, measured, predicted, rel) in &errors {
        assert!(
            *rel < 0.03,
            "unpowered coast follows omega0 * exp(-t / (J/B)) with tau = {tau:.3} s: at \
             t = {} ms omega = {measured:.4} rad/s vs {predicted:.4} rad/s ({:.3}% error)",
            *k + 1,
            rel * 100.0
        );
    }
}

#[test]
fn svpwm_common_mode_rejected() {
    // The floating neutral absorbs any duty offset applied to all three legs, so a
    // common-mode shift leaves the phase currents and torque bit-for-bit unmoved.
    const TICKS: usize = 200;
    const BASE: [f64; 3] = [0.5, 0.6, 0.4];
    const OFFSET: f64 = 0.2;
    const TOLERANCE: f64 = 1.0e-6;

    let params = MotorParams {
        r_ohm: 32.0,
        l_h: 1.0e-3,
        ke_v_per_mech_rad_s: 0.05,
        pole_pairs: 2,
        j_kg_m2: 1.0e-4,
        b_nm_per_rad_s: 1.0e-3,
        t_c_nm: 0.0,
        v_d: 0.8,
    };
    let mut plain = world(params, 0.3, VBUS_V);
    let mut shifted = world(params, 0.3, VBUS_V);
    drive(&mut plain, BASE, [true; 3], true);
    drive(
        &mut shifted,
        [BASE[0] + OFFSET, BASE[1] + OFFSET, BASE[2] + OFFSET],
        [true; 3],
        true,
    );

    let mut worst_current = 0.0f64;
    let mut worst_torque = 0.0f64;
    for _ in 0..TICKS {
        plain.step().expect("engine step");
        shifted.step().expect("engine step");
        let (a, b) = (sample(&plain), sample(&shifted));
        for (x, y) in a.current.iter().zip(b.current.iter()) {
            worst_current = worst_current.max((x - y).abs());
        }
        worst_torque = worst_torque.max((a.torque - b.torque).abs());
    }
    let end = sample(&plain);
    println!(
        "svpwm_common_mode_rejected: worst current delta = {worst_current:.3e} A, worst torque \
         delta = {worst_torque:.3e} Nm over {TICKS} ticks (tolerance {TOLERANCE:.0e}); \
         reference currents {:?}, torque {:.6} Nm",
        end.current, end.torque
    );

    assert!(
        worst_current <= TOLERANCE,
        "a common-mode duty offset leaves the phase currents untouched: worst delta \
         {worst_current:.3e} A"
    );
    assert!(
        worst_torque <= TOLERANCE,
        "a common-mode duty offset leaves the torque untouched: worst delta \
         {worst_torque:.3e} Nm"
    );
}

#[test]
fn overspeed_coast_engages_diodes() {
    // Collapsing the bus below the line-to-line BEMF turns the bridge into a rectifier: the
    // body diodes conduct with no gate drive and the regen torque brakes past friction.
    const SPIN_TICKS: usize = 800;
    const COLLAPSED_VBUS_V: f64 = 1.0;
    const CONDUCTION_FLOOR_A: f64 = 0.25;
    const QUIET_TICKS: usize = 20;

    let params = spin_params();
    let tau = tau_mech_s(&params);
    // The all-open window check is per-terminal (v_n = 0 convention), so marginal
    // micro-conduction persists until per-phase BEMF falls under v_d — quiet is only
    // assertable below that band.
    let per_terminal_onset_rad_s = params.v_d / params.ke_v_per_mech_rad_s;
    let quiet_entry_rad_s = 0.95 * per_terminal_onset_rad_s;

    let mut eng = world(params, PI / 8.0, VBUS_V);
    let mut angle = PI / 8.0;
    for _ in 0..SPIN_TICKS {
        commutate(&mut eng, angle, SPIN_POLE_PAIRS, SPIN_DUTY);
        eng.step().expect("engine step");
        angle = sample(&eng).angle;
    }
    let spinning = sample(&eng);
    let w0 = spinning.velocity;
    let line_to_line_bemf = 2.0 * params.ke_v_per_mech_rad_s * w0;
    let diode_window = COLLAPSED_VBUS_V + 2.0 * params.v_d;
    assert!(
        line_to_line_bemf > diode_window,
        "the coast starts above the rectification threshold: BEMF {line_to_line_bemf:.3} V \
         vs window {diode_window:.3} V"
    );

    // One tick collapses the bus and releases the bridge together.
    eng.write(&vid(MOTOR, "v_bus"), COLLAPSED_VBUS_V)
        .expect("write collapsed vbus");
    drive(&mut eng, [0.0; 3], [false; 3], false);

    // Coast until the rotor falls below the engagement band, capped at 20 tau.
    let cap_ticks = ((20.0 * tau / TICK_S) as usize).max(1_000);
    let mut trace: Vec<Sample> = Vec::new();
    while trace.last().is_none_or(|s| s.velocity >= quiet_entry_rad_s) {
        assert!(
            trace.len() < cap_ticks,
            "the rotor slows below the engagement band ({quiet_entry_rad_s:.3} rad/s) \
             within 20 tau ({cap_ticks} ticks)"
        );
        eng.step().expect("engine step");
        trace.push(sample(&eng));
    }
    let coast_ticks = trace.len();

    let peak_current = trace.iter().fold(0.0f64, |m, s| m.max(s.peak_current()));
    // Regen braking outruns friction: the drop to 0.8 omega0 arrives in under half the
    // time friction alone needs (tau * ln(1/0.8)).
    let brake_ticks = trace
        .iter()
        .position(|s| s.velocity < (0.8 * w0))
        .expect("the coast passes 0.8 omega0 on its way below the engagement band")
        + 1;
    let brake_time_s = (brake_ticks as f64) * TICK_S;
    let friction_time_s = tau * (1.0f64 / 0.8).ln();

    // Below the band nothing may conduct: every current reads exactly zero.
    let mut quiet: Vec<f64> = Vec::with_capacity(QUIET_TICKS);
    for _ in 0..QUIET_TICKS {
        eng.step().expect("engine step");
        quiet.push(sample(&eng).peak_current());
    }
    println!(
        "overspeed_coast_engages_diodes: omega0 = {w0:.3} rad/s (BEMF {line_to_line_bemf:.3} V \
         vs {diode_window:.3} V window), peak coast current = {peak_current:.4} A, 0.8 omega0 \
         at {:.1} ms vs {:.1} ms friction-only, band exit after {coast_ticks} ticks at \
         {:.3} rad/s",
        brake_time_s * 1e3,
        friction_time_s * 1e3,
        trace[coast_ticks - 1].velocity
    );

    assert!(
        peak_current > CONDUCTION_FLOOR_A,
        "the body diodes rectify the BEMF into the collapsed bus: peak coast current \
         {peak_current:.4} A"
    );
    assert!(
        brake_time_s < (0.5 * friction_time_s),
        "regen braking reaches 0.8 omega0 in under half the friction-only time: \
         {:.1} ms vs {:.1} ms",
        brake_time_s * 1e3,
        friction_time_s * 1e3
    );
    assert!(
        quiet.iter().all(|c| *c == 0.0),
        "conduction stops once the BEMF falls back inside the window: {QUIET_TICKS} ticks \
         below the band read {quiet:?}"
    );
}

/// The bench-measured bus voltage the Coulomb tests drive with (PD contract).
const MEASURED_VBUS_V: f64 = 19.57;

/// Alignment-vector spring stiffness at equilibrium for a U-high/V-low pair on the
/// sinusoidal machine: d(Te)/d(theta_mech) = sqrt(3)·Ke·I·p.
fn align_spring(p: &MotorParams, duty: f64, vbus: f64) -> f64 {
    let i = duty * vbus / (2.0 * p.r_ohm);
    3.0_f64.sqrt() * p.ke_v_per_mech_rad_s * i * f64::from(p.pole_pairs)
}

#[test]
fn stiction_parks_rotor_exactly() {
    // With Coulomb friction the settled rotor is parked: velocity exactly zero, angle
    // bit-frozen, within the T_c/K stick band of the alignment equilibrium.
    const DUTY: f64 = 0.1;
    const SETTLE_TICKS: usize = 600;
    const FROZEN_TICKS: usize = 50;

    let p = MotorParams::default();
    let eq = (5.0 * PI / 6.0) / f64::from(p.pole_pairs);
    let start = eq - 0.10;
    let stick_band = p.t_c_nm / align_spring(&p, DUTY, MEASURED_VBUS_V);

    let mut eng = world(p, start, MEASURED_VBUS_V);
    drive(&mut eng, [DUTY, 0.0, 0.0], [true, true, false], true);
    step_n(&mut eng, SETTLE_TICKS);
    let settled = sample(&eng);
    let mut frozen = true;
    for _ in 0..FROZEN_TICKS {
        eng.step().expect("engine step");
        let s = sample(&eng);
        frozen &= (s.angle == settled.angle) && (s.velocity == 0.0);
    }
    println!(
        "stiction_parks_rotor_exactly: settled {:.6} rad vs eq {eq:.6} (stick band \
         ±{stick_band:.6}), velocity {:.3e}",
        settled.angle, settled.velocity
    );

    assert_eq!(
        settled.velocity, 0.0,
        "the parked rotor is exactly at rest: {:.3e} rad/s",
        settled.velocity
    );
    assert!(
        frozen,
        "the parked angle is bit-frozen across {FROZEN_TICKS} ticks"
    );
    assert!(
        (settled.angle - eq).abs() < (stick_band + 0.02),
        "the park lands within the stick band of equilibrium: {:.6} rad vs {eq:.6} \
         ±{stick_band:.6}",
        settled.angle
    );
}

#[test]
fn breakaway_threshold_matches_coulomb() {
    // At the max-torque rotor position the drive breaks stiction only above
    // duty* = 2·R·T_c / (sqrt(3)·Ke·vbus): below it the rotor never moves at all.
    const HOLD_TICKS: usize = 200;

    let p = MotorParams::default();
    let eq = (5.0 * PI / 6.0) / f64::from(p.pole_pairs);
    let start = eq - (PI / 2.0) / f64::from(p.pole_pairs); // 90° electrical: max torque
    let duty_star =
        2.0 * p.r_ohm * p.t_c_nm / (3.0_f64.sqrt() * p.ke_v_per_mech_rad_s * MEASURED_VBUS_V);
    println!("breakaway_threshold_matches_coulomb: duty* = {duty_star:.5}");

    let mut below = world(p, start, MEASURED_VBUS_V);
    drive(
        &mut below,
        [0.6 * duty_star, 0.0, 0.0],
        [true, true, false],
        true,
    );
    step_n(&mut below, HOLD_TICKS);
    let s = sample(&below);
    assert_eq!(
        (s.angle, s.velocity),
        (start, 0.0),
        "under-threshold drive never moves the rotor: {:?}",
        (s.angle, s.velocity)
    );

    let mut above = world(p, start, MEASURED_VBUS_V);
    drive(
        &mut above,
        [2.0 * duty_star, 0.0, 0.0],
        [true, true, false],
        true,
    );
    step_n(&mut above, HOLD_TICKS);
    let s = sample(&above);
    assert!(
        (s.angle - start).abs() > 1.0e-3,
        "over-threshold drive breaks the rotor away: moved {:.3e} rad",
        (s.angle - start).abs()
    );
}

#[test]
fn coast_stops_exactly_in_finite_time() {
    // Coulomb friction ends a coast at exact zero within the analytic
    // t = (J/B)·ln(1 + B·omega0/T_c), where pure viscous decay never truly stops.
    const SPIN_TICKS: usize = 800;
    const QUIET_TICKS: usize = 50;

    let p = MotorParams::default();
    let mut eng = world(p, PI / 8.0, MEASURED_VBUS_V);
    let mut angle = PI / 8.0;
    for _ in 0..SPIN_TICKS {
        commutate(&mut eng, angle, p.pole_pairs, 0.9);
        eng.step().expect("engine step");
        angle = sample(&eng).angle;
    }
    let w0 = sample(&eng).velocity;
    assert!(
        w0 > 3.0,
        "six-step spins the measured motor up: {w0:.3} rad/s"
    );

    drive(&mut eng, [0.0; 3], [false; 3], false);
    let t_stop_s = (p.j_kg_m2 / p.b_nm_per_rad_s) * (1.0 + p.b_nm_per_rad_s * w0 / p.t_c_nm).ln();
    let budget = (2.0 * t_stop_s / TICK_S) as usize;
    let mut stopped_at = None;
    for k in 0..budget {
        eng.step().expect("engine step");
        if sample(&eng).velocity == 0.0 {
            stopped_at = Some(k);
            break;
        }
    }
    let k_stop = stopped_at.unwrap_or_else(|| {
        panic!(
            "the coast reaches exactly zero within 2x the analytic {t_stop_s:.3} s \
             (omega0 = {w0:.3})"
        )
    });
    println!(
        "coast_stops_exactly_in_finite_time: omega0 = {w0:.3} rad/s, stopped at \
         {:.3} s vs analytic {t_stop_s:.3} s",
        (k_stop as f64) * TICK_S
    );
    for _ in 0..QUIET_TICKS {
        eng.step().expect("engine step");
        assert_eq!(sample(&eng).velocity, 0.0, "the stopped rotor stays parked");
    }
}
