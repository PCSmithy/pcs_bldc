//! BLDC motor + averaged-duty inverter (D6) — the plant that closes the commutation loop.
//!
//! All ports live in the model's own namespace; the sim wiring routes the board's
//! bridge signals into them. Inputs: `vsig:<name>:{duty,enable}_{u,v,w}`, `moe`,
//! `vbus` — per-leg normalized duty in [0,1], 0/1 enables, master output enable, bus
//! voltage. Outputs: `angle`, `velocity`, `phase_current_*`, `torque`,
//! `terminal_voltage_*`, `bemf_*`, `neutral_voltage`.
//!
//! Each leg runs a hybrid mode machine — driven (duty × vbus), diode-clamped to a
//! rail, or open — over a trapezoidal-BEMF R/L electrical model about a floating
//! neutral, plus J/B mechanics, integrated semi-implicitly at
//! [`MOTOR_INTEGRATOR_STEP_PERIOD_US`] sub-steps. A disabled leg freewheels through
//! its body diode to exact zero then floats; a floating terminal re-clamps if it
//! leaves the rail window.

use std::f64::consts::{PI, TAU};

use voyant::{vsig_id, Member, MemberCtx, SignalId, StateTable, Value};

pub const MOTOR_MODEL_STEP_PERIOD_US: u16 = 1000;
pub const MOTOR_INTEGRATOR_STEP_PERIOD_US: u16 = 1;
// The integrator step must evenly divide the model tick (u16 division truncates).
#[allow(clippy::modulo_one)] // the divisor is a placeholder; the guard exists for when it changes
const _: () = assert!(MOTOR_MODEL_STEP_PERIOD_US.is_multiple_of(MOTOR_INTEGRATOR_STEP_PERIOD_US));

const DIODE_ENGAGE_MARGIN_V: f64 = 0.005;

/// STSPIN32G4 target motor pole-pair count.
pub const DEFAULT_POLE_PAIRS: u8 = 14;

/// Electrical + mechanical motor parameters. Every value is a **TODO(owner)**
/// placeholder — the owner fills these from the real motor + bus.
#[derive(Clone, Copy, Debug)]
pub struct MotorParams {
    /// Per-phase winding resistance (Ω). TODO(owner).
    pub r_ohm: f64,
    /// Per-phase winding inductance (H). TODO(owner).
    pub l_h: f64,
    /// Back-EMF constant (V per rad/s, mechanical). TODO(owner).
    pub ke_v_per_mech_rad_s: f64,

    /// Rotor pole pairs (electrical revs per mechanical rev).
    pub pole_pairs: u8,
    /// Rotor + load inertia (kg·m²). TODO(owner).
    pub j_kg_m2: f64,
    /// Viscous friction (Nm per rad/s). TODO(owner).
    pub b_nm_per_rad_s: f64,
    /// Forward voltage drop over the body diodes of the bridge FETs
    pub v_d: f64,
}

impl Default for MotorParams {
    fn default() -> Self {
        // TODO(owner): real motor + bus values; these are placeholders only.
        Self {
            r_ohm: 32.0,
            l_h: 1.0e-3,
            ke_v_per_mech_rad_s: 0.01,
            pole_pairs: DEFAULT_POLE_PAIRS,
            j_kg_m2: 1.0e-4,
            b_nm_per_rad_s: 1.0e-4,
            v_d: 0.8,
        }
    }
}

#[derive(Clone, Copy, PartialEq)]
pub enum Rail {
    Low,
    High,
}

#[derive(Clone, Copy, PartialEq)]
pub enum LegMode {
    Driven,
    Clamped(Rail),
    Open,
}

/// A trapezoidal-BEMF BLDC motor driven by an averaged-duty inverter. Reads the bridge
/// command + bus voltage from its input ports each tick and records the plant state on
/// its output ports (see the module doc for the port list).
pub struct MotorModel {
    name: String,
    params: MotorParams,

    // Integrated plant state.
    angle_rad: f64,
    velocity_rad_s: f64,
    phase_current_a: [f64; 3],
    torque_nm: f64,

    modes: [LegMode; 3],
}

const N_PHASES: usize = 3;

fn bemf_shape(theta_e: f64) -> f64 {
    debug_assert!(theta_e.is_finite());

    // Ideal trapezoid; a bench-fitted shape can replace it.
    let t = theta_e.rem_euclid(TAU);
    if t < PI / 6.0 {
        (6.0 / PI) * t
    } else if t < 5.0 * PI / 6.0 {
        1.0
    } else if t < 7.0 * PI / 6.0 {
        (6.0 / PI) * (PI - t)
    } else if t < 11.0 * PI / 6.0 {
        -1.0
    } else {
        (6.0 / PI) * (t - TAU)
    }
}

impl MotorModel {
    /// A motor named `name`, its shaft at `initial_angle_rad`, with placeholder
    /// [`MotorParams`].
    pub fn new(name: &str, initial_angle_rad: f64) -> Self {
        Self {
            name: name.to_string(),
            params: MotorParams::default(),
            angle_rad: initial_angle_rad,
            velocity_rad_s: 0.0,
            phase_current_a: [0.0; 3],
            torque_nm: 0.0,
            modes: [LegMode::Open; 3],
        }
    }

    /// Override the placeholder parameters.
    pub fn with_params(mut self, params: MotorParams) -> Self {
        self.params = params;
        self
    }

    fn out_id(&self, local: &str) -> SignalId {
        vsig_id(&self.name, local).expect("valid vsig id")
    }

    /// One input port's current value (`0.0` when never driven / not numeric — a
    /// dark bridge reads all zeros).
    fn observe(st: &StateTable, source: &str, local: &str) -> f64 {
        SignalId::new("vsig", source, local, None)
            .ok()
            .and_then(|id| st.current_value(&id).ok().flatten())
            .and_then(|v| v.as_f64())
            .unwrap_or(0.0)
    }
}

impl Member for MotorModel {
    fn name(&self) -> &str {
        &self.name
    }

    fn advance(&mut self, dt_us: u64, ctx: &mut MemberCtx) {
        debug_assert_eq!(dt_us, MOTOR_MODEL_STEP_PERIOD_US as u64);

        // Input ports: the bridge command + bus voltage the sim wiring routed in.
        let vbus = Self::observe(ctx.st, &self.name, "vbus");

        let ob = |local: &str| Self::observe(ctx.st, &self.name, local);
        let duty = ["duty_u", "duty_v", "duty_w"].map(&ob);
        let enabled = ["enable_u", "enable_v", "enable_w"].map(|s| ob(s) != 0.0);
        let master_output_enabled = ob("moe") != 0.0;

        let mut v: [f64; 3] = [0.0; 3]; // terminal voltage
        let mut e: [f64; 3] = [0.0; 3]; // back emf
        let mut v_n = 0.0;

        let n = MOTOR_MODEL_STEP_PERIOD_US / MOTOR_INTEGRATOR_STEP_PERIOD_US; // number of integrator sub-steps per model step
        let dt_s = f64::from(MOTOR_INTEGRATOR_STEP_PERIOD_US) * 1e-6;

        // per integrator sub-step
        for _ in 0..n {
            // 1. resolve modes from bridge command

            for (k, &en) in enabled.iter().enumerate() {
                if master_output_enabled && en {
                    self.modes[k] = LegMode::Driven;
                } else if self.modes[k] == LegMode::Driven {
                    // not enabled
                    if self.phase_current_a[k].abs() > f64::EPSILON {
                        // non-zero phase current - freewheeling diode
                        self.modes[k] = if self.phase_current_a[k] > 0.0 {
                            LegMode::Clamped(Rail::Low)
                        } else {
                            LegMode::Clamped(Rail::High)
                        };
                    } else {
                        self.modes[k] = LegMode::Open;
                        // zero phase current - floating node
                        // will compute v[k] after back emf is known
                    }
                }
                // otherwise - keep previous mode
                // Clamped exits via step 4
                // Open exits via step 5
            }

            let num_enabled = self
                .modes
                .iter()
                .filter(|m| !matches!(m, LegMode::Open))
                .count();
            for k in 0..N_PHASES {
                v[k] = match self.modes[k] {
                    LegMode::Driven => duty[k] * vbus,
                    LegMode::Clamped(Rail::High) => vbus + self.params.v_d,
                    LegMode::Clamped(Rail::Low) => -self.params.v_d,
                    LegMode::Open => v[k], // filled in after v_n is determined
                }
            }

            // 2. compute leg back-emf, and the common-node voltage v_n.
            // |D| = 0 (all legs open): v_n = 0 by convention, so float voltages are just
            // e[k]; the step-5 per-terminal window is then approximate (exact onset:
            // e_max − e_min > vbus + 2·v_d).
            v_n = 0.0;

            let theta_e = self.angle_rad * f64::from(self.params.pole_pairs);
            let f_shape = [
                //  normalized BEMF shape (dλ/dθe, peak ±1, dimensionless)
                bemf_shape(theta_e),
                bemf_shape(theta_e - TAU / 3.0),
                bemf_shape(theta_e - 4.0 * PI / 3.0),
            ];
            for k in 0..N_PHASES {
                e[k] = self.params.ke_v_per_mech_rad_s * f_shape[k] * self.velocity_rad_s;

                if self.modes[k] != LegMode::Open {
                    v_n += v[k] - e[k];
                }
            }
            if num_enabled > 0 {
                v_n /= num_enabled as f64;
            }

            for k in 0..N_PHASES {
                if self.modes[k] == LegMode::Open {
                    v[k] = e[k] + v_n;
                }
            }

            // 3. integrate di/dt per leg, dw/dt, dtheta/dt
            let mut sum_flux: f64 = 0.0;

            for k in 0..N_PHASES {
                if self.modes[k] != LegMode::Open {
                    // is there a better integration method than this?
                    let di_dt = (v[k] - v_n - e[k] - self.params.r_ohm * self.phase_current_a[k])
                        / self.params.l_h;
                    self.phase_current_a[k] += di_dt * dt_s;

                    sum_flux += f_shape[k] * self.phase_current_a[k];
                }
            }
            self.torque_nm = self.params.ke_v_per_mech_rad_s * sum_flux;

            // sum shaft torques - add to sum with T_cog, T_load, etc.
            self.velocity_rad_s += (self.torque_nm
                - self.params.b_nm_per_rad_s * self.velocity_rad_s)
                / self.params.j_kg_m2
                * dt_s;

            self.angle_rad = (self.angle_rad + self.velocity_rad_s * dt_s).rem_euclid(TAU);

            // 4. check leg ideal diode regimes - current clamp
            // Exit when the current crosses against the diode direction OR its freewheel
            // tail decays below epsilon (an asymptotic decay never crosses exact zero).
            for k in 0..N_PHASES {
                let exited = match self.modes[k] {
                    LegMode::Clamped(Rail::Low) => self.phase_current_a[k] < f64::EPSILON,
                    LegMode::Clamped(Rail::High) => self.phase_current_a[k] > -f64::EPSILON,
                    _ => false,
                };
                if exited {
                    self.modes[k] = LegMode::Open;
                    self.phase_current_a[k] = 0.0;
                }
            }

            // 5. check leg ideal diode regimes - voltage clamp
            let low_engage = -(self.params.v_d + DIODE_ENGAGE_MARGIN_V);
            let high_engage = vbus + self.params.v_d + DIODE_ENGAGE_MARGIN_V;

            for (mode, &v_k) in self.modes.iter_mut().zip(&v) {
                if *mode == LegMode::Open {
                    if v_k < low_engage {
                        *mode = LegMode::Clamped(Rail::Low);
                    } else if v_k > high_engage {
                        *mode = LegMode::Clamped(Rail::High);
                    }
                    // else: still floating
                }
            }
        }

        // Record the tick's outputs (last sub-step sample).
        let _ = ctx
            .st
            .record(&self.out_id("angle"), Value::F64(self.angle_rad));
        let _ = ctx
            .st
            .record(&self.out_id("velocity"), Value::F64(self.velocity_rad_s));
        let _ = ctx
            .st
            .record(&self.out_id("torque"), Value::F64(self.torque_nm));

        for (c, i) in [("u", 0), ("v", 1), ("w", 2)] {
            let _ = ctx.st.record(
                &self.out_id(&format!("phase_current_{c}")),
                Value::F64(self.phase_current_a[i]),
            );
            let _ = ctx.st.record(
                &self.out_id(&format!("terminal_voltage_{c}")),
                Value::F64(v[i]),
            );
            let _ = ctx
                .st
                .record(&self.out_id(&format!("bemf_{c}")), Value::F64(e[i]));
        }

        let _ = ctx
            .st
            .record(&self.out_id("neutral_voltage"), Value::F64(v_n));
    }

    fn set_enabled(&mut self, on: bool, st: &mut StateTable) {
        if on {
            // inputs
            let _ = st.register(self.out_id("vbus"), Some("V"));
            let _ = st.register(self.out_id("duty_u"), None);
            let _ = st.register(self.out_id("duty_v"), None);
            let _ = st.register(self.out_id("duty_w"), None);
            let _ = st.register(self.out_id("enable_u"), None);
            let _ = st.register(self.out_id("enable_v"), None);
            let _ = st.register(self.out_id("enable_w"), None);
            let _ = st.register(self.out_id("moe"), None);

            // outputs
            let _ = st.register(self.out_id("angle"), Some("rad"));
            let _ = st.register(self.out_id("velocity"), Some("rad/s"));
            let _ = st.register(self.out_id("phase_current_u"), Some("A"));
            let _ = st.register(self.out_id("phase_current_v"), Some("A"));
            let _ = st.register(self.out_id("phase_current_w"), Some("A"));
            let _ = st.register(self.out_id("torque"), Some("Nm"));
            let _ = st.register(self.out_id("terminal_voltage_u"), Some("V"));
            let _ = st.register(self.out_id("terminal_voltage_v"), Some("V"));
            let _ = st.register(self.out_id("terminal_voltage_w"), Some("V"));
            let _ = st.register(self.out_id("neutral_voltage"), Some("V"));
            let _ = st.register(self.out_id("bemf_u"), Some("V"));
            let _ = st.register(self.out_id("bemf_v"), Some("V"));
            let _ = st.register(self.out_id("bemf_w"), Some("V"));
        }
    }
}
