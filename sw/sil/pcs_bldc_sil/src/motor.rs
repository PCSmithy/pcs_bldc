//! BLDC motor + averaged-duty inverter — the plant that closes the commutation loop.
//!
//! All ports live in the model's own namespace; the sim wiring routes the board's
//! bridge signals into them. Inputs: `vsig:<name>:{duty,enable}_{u,v,w}`, `moe`,
//! `v_bus` — per-leg normalized duty in [0,1], 0/1 enables, master output enable, bus
//! voltage. Outputs: `angle`, `velocity`, `phase_current_*`, `bus_current`, `torque`,
//! `terminal_voltage_*`, `bemf_*`, `neutral_voltage`.
//!
//! Each leg runs a hybrid mode machine — driven (duty × v_bus), diode-clamped to a
//! rail, or open — over a sinusoidal-BEMF (PMSM) R/L electrical model about a
//! floating neutral, plus J/B/Coulomb mechanics with a stick state at zero speed,
//! integrated semi-implicitly at
//! [`MOTOR_INTEGRATOR_STEP_PERIOD_US`] sub-steps. A disabled leg freewheels through
//! its body diode to exact zero then floats; a floating terminal re-clamps if it
//! leaves the rail window.

use std::f64::consts::{PI, TAU};

use voyant::{vsig_id, Member, MemberCtx, SignalId, StateTable, Value};

pub const MOTOR_INTEGRATOR_STEP_PERIOD_US: u16 = 1;

const DIODE_ENGAGE_MARGIN_V: f64 = 0.005;

/// STSPIN32G4 target motor pole-pair count.
pub const DEFAULT_POLE_PAIRS: u8 = 14;

/// Electrical + mechanical motor parameters. Defaults are the bench-measured
/// iPower GM6208-150T values (`tools/trace_analysis/*`); `l_h` is a class-typical
/// estimate (its time constant sits below the capture bandwidth).
#[derive(Clone, Copy, Debug)]
pub struct MotorParams {
    /// Per-phase winding resistance (Ω).
    pub r_ohm: f64,
    /// Per-phase winding inductance (H).
    pub l_h: f64,
    /// Back-EMF constant (V per rad/s, mechanical).
    pub ke_v_per_mech_rad_s: f64,

    /// Rotor pole pairs (electrical revs per mechanical rev).
    pub pole_pairs: u8,
    /// Rotor + load inertia (kg·m²).
    pub j_kg_m2: f64,
    /// Viscous friction (Nm per rad/s).
    pub b_nm_per_rad_s: f64,
    /// Coulomb friction torque (Nm); breakaway equals it — the stick state holds
    /// the rotor until the drive torque exceeds it.
    pub t_c_nm: f64,
    /// Forward voltage drop over the body diodes of the bridge FETs
    pub v_d: f64,
}

impl Default for MotorParams {
    fn default() -> Self {
        Self {
            r_ohm: 14.7,
            l_h: 5.0e-3,
            ke_v_per_mech_rad_s: 0.55,
            pole_pairs: DEFAULT_POLE_PAIRS,
            j_kg_m2: 3.0e-4,
            b_nm_per_rad_s: 7.2e-4,
            t_c_nm: 4.4e-3,
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

/// A sinusoidal-BEMF PM motor driven by an averaged-duty inverter. Reads the bridge
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

    // Sinusoidal machine (bench-measured, 4.1% rms vs pure sine —
    // tools/trace_analysis/bemf_spin).
    let t = theta_e.rem_euclid(TAU);
    t.sin()
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

    fn port_id(&self, local: &str) -> SignalId {
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
        // The integrator step must evenly divide the model tick
        debug_assert!(dt_us.is_multiple_of(MOTOR_INTEGRATOR_STEP_PERIOD_US as u64));

        // Input ports: the bridge command + bus voltage the sim wiring routed in.
        let v_bus = Self::observe(ctx.st, &self.name, "v_bus");
        let mut i_bus = 0.0;

        let ob = |local: &str| Self::observe(ctx.st, &self.name, local);
        let duty = ["duty_u", "duty_v", "duty_w"].map(&ob);
        let enabled = ["enable_u", "enable_v", "enable_w"].map(|s| ob(s) != 0.0);
        let master_output_enabled = ob("moe") != 0.0;

        let mut v: [f64; 3] = [0.0; 3]; // terminal voltage
        let mut e: [f64; 3] = [0.0; 3]; // back emf
        let mut v_n = 0.0;

        let n = dt_us / MOTOR_INTEGRATOR_STEP_PERIOD_US as u64; // number of integrator sub-steps per model step
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
                    LegMode::Driven => duty[k] * v_bus,
                    LegMode::Clamped(Rail::High) => v_bus + self.params.v_d,
                    LegMode::Clamped(Rail::Low) => -self.params.v_d,
                    LegMode::Open => v[k], // filled in after v_n is determined
                }
            }

            // 2. compute leg back-emf, and the common-node voltage v_n.
            // |D| = 0 (all legs open): v_n = 0 by convention, so float voltages are just
            // e[k]; the step-5 per-terminal window is then approximate (exact onset:
            // e_max − e_min > v_bus + 2·v_d).
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
            let t_drive = self.torque_nm; // TODO: + t_cog - t_load
            if (self.velocity_rad_s == 0.0) && (t_drive.abs() <= self.params.t_c_nm) {
                // stuck - need more drive to overcome static friction
            } else {
                // Kinetic friction opposes motion — or, at breakaway from rest, the
                // incipient motion (signum(0) is +1, never use it here).
                let dir = if self.velocity_rad_s != 0.0 {
                    self.velocity_rad_s.signum()
                } else {
                    t_drive.signum()
                };
                let t_net = t_drive
                    - self.params.b_nm_per_rad_s * self.velocity_rad_s
                    - self.params.t_c_nm * dir;
                let w_new = self.velocity_rad_s + t_net / self.params.j_kg_m2 * dt_s;
                // Friction cannot carry the rotor through zero: a sign flip sticks at
                // exactly 0.0; breakaway re-evaluates next sub-step.
                self.velocity_rad_s = if (w_new * self.velocity_rad_s) < 0.0 {
                    0.0
                } else {
                    w_new
                };
            }
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
            // Average DC-link current: a driven leg draws duty's worth of its phase
            // current from the bus; a high-clamped leg ties its node to the bus through
            // the diode (negative during regen); low-clamped and open legs never touch it.
            i_bus = 0.0;
            for ((mode, &d), &i) in self.modes.iter().zip(&duty).zip(&self.phase_current_a) {
                i_bus += match mode {
                    LegMode::Clamped(Rail::High) => i,
                    LegMode::Driven => i * d,
                    LegMode::Clamped(Rail::Low) | LegMode::Open => 0.0,
                };
            }

            // 5. check leg ideal diode regimes - voltage clamp
            let low_engage = -(self.params.v_d + DIODE_ENGAGE_MARGIN_V);
            let high_engage = v_bus + self.params.v_d + DIODE_ENGAGE_MARGIN_V;

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
            .record(&self.port_id("angle"), Value::F64(self.angle_rad));
        let _ = ctx
            .st
            .record(&self.port_id("velocity"), Value::F64(self.velocity_rad_s));
        let _ = ctx
            .st
            .record(&self.port_id("torque"), Value::F64(self.torque_nm));

        for (c, i) in [("u", 0), ("v", 1), ("w", 2)] {
            let _ = ctx.st.record(
                &self.port_id(&format!("phase_current_{c}")),
                Value::F64(self.phase_current_a[i]),
            );
            let _ = ctx.st.record(
                &self.port_id(&format!("terminal_voltage_{c}")),
                Value::F64(v[i]),
            );
            let _ = ctx
                .st
                .record(&self.port_id(&format!("bemf_{c}")), Value::F64(e[i]));
        }

        let _ = ctx
            .st
            .record(&self.port_id("bus_current"), Value::F64(i_bus));

        let _ = ctx
            .st
            .record(&self.port_id("neutral_voltage"), Value::F64(v_n));
    }

    fn set_enabled(&mut self, on: bool, st: &mut StateTable) {
        if on {
            // inputs
            let _ = st.register(self.port_id("v_bus"), Some("V"));
            let _ = st.register(self.port_id("duty_u"), None);
            let _ = st.register(self.port_id("duty_v"), None);
            let _ = st.register(self.port_id("duty_w"), None);
            let _ = st.register(self.port_id("enable_u"), None);
            let _ = st.register(self.port_id("enable_v"), None);
            let _ = st.register(self.port_id("enable_w"), None);
            let _ = st.register(self.port_id("moe"), None);

            // outputs
            let _ = st.register(self.port_id("angle"), Some("rad"));
            let _ = st.register(self.port_id("velocity"), Some("rad/s"));
            let _ = st.register(self.port_id("phase_current_u"), Some("A"));
            let _ = st.register(self.port_id("phase_current_v"), Some("A"));
            let _ = st.register(self.port_id("phase_current_w"), Some("A"));
            let _ = st.register(self.port_id("bus_current"), Some("A"));
            let _ = st.register(self.port_id("torque"), Some("Nm"));
            let _ = st.register(self.port_id("terminal_voltage_u"), Some("V"));
            let _ = st.register(self.port_id("terminal_voltage_v"), Some("V"));
            let _ = st.register(self.port_id("terminal_voltage_w"), Some("V"));
            let _ = st.register(self.port_id("neutral_voltage"), Some("V"));
            let _ = st.register(self.port_id("bemf_u"), Some("V"));
            let _ = st.register(self.port_id("bemf_v"), Some("V"));
            let _ = st.register(self.port_id("bemf_w"), Some("V"));
        }
    }
}
