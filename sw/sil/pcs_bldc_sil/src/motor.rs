//! BLDC motor + inverter model scaffolding — the plant that closes the commutation
//! loop. Every connection point is wired; every equation is a `TODO(owner)`.
//!
//! D6 contract: the firmware's commanded bridge state arrives as an averaged-duty
//! inverter input — per-phase duty in [0,1], per-phase enable, and one master output
//! enable (`vsig:pcs_bldc:PWM_{U,V,W}_{duty,enabled}` + `TIM1_MOE`). A phase whose
//! enable is low floats (six-step leaves one leg tri-stated). Duty × Vbus sets the
//! driven phase voltages; the model owns its integrator and sub-steps when the
//! electrical time constant τ = L/R is short against the tick. The mechanical state
//! integrates torque against inertia + friction; the shaft `angle` output routes into
//! the AS5048 encoder model, which quantizes it back to the firmware over SPI.

use crate::SOURCE;
use voyant::{vsig_id, Member, MemberCtx, SignalId, StateTable, Value};

/// STSPIN32G4 target motor pole-pair count.
pub const DEFAULT_POLE_PAIRS: u8 = 14;

/// Electrical + mechanical motor parameters. Every value is a **TODO(owner)**
/// placeholder — the owner fills these from the real motor + bus.
#[derive(Clone, Copy, Debug)]
pub struct MotorParams {
    /// Per-phase winding resistance (Ω). TODO(owner).
    pub r_ohm: f32,
    /// Per-phase winding inductance (H). TODO(owner).
    pub l_h: f32,
    /// Back-EMF constant (V per rad/s, electrical). TODO(owner).
    pub ke_v_per_rad_s: f32,
    /// Torque constant (Nm per A). TODO(owner).
    pub kt_nm_per_a: f32,
    /// Rotor pole pairs (electrical revs per mechanical rev).
    pub pole_pairs: u8,
    /// Rotor + load inertia (kg·m²). TODO(owner).
    pub j_kg_m2: f32,
    /// Viscous friction (Nm per rad/s). TODO(owner).
    pub b_nm_per_rad_s: f32,
    /// DC-bus voltage the inverter switches (V). TODO(owner).
    pub vbus_v: f32,
}

impl Default for MotorParams {
    fn default() -> Self {
        // TODO(owner): real motor + bus values; these are placeholders only.
        Self {
            r_ohm: 1.0,
            l_h: 1.0e-3,
            ke_v_per_rad_s: 0.01,
            kt_nm_per_a: 0.01,
            pole_pairs: DEFAULT_POLE_PAIRS,
            j_kg_m2: 1.0e-5,
            b_nm_per_rad_s: 1.0e-4,
            vbus_v: 24.0,
        }
    }
}

/// A trapezoidal-BEMF BLDC motor driven by an averaged-duty inverter. Registers its
/// mechanical + electrical state as `vsig` outputs (`angle`, `velocity`,
/// `phase_current_{u,v,w}`, `torque`) and, each tick, reads the firmware's bridge
/// commands. The dynamics are the owner's to write; this scaffold wires the seams and
/// holds the state at its initial values. // TODO - remove all these "owner" comments, any narrative comments before merging back to main
pub struct MotorModel {
    name: String,
    /// The `<source>` whose `vsig:<source>:PWM_*` ports carry the bridge command —
    /// the firmware instance driving this motor.
    bridge_source: String,
    params: MotorParams,

    // State the owner integrates. TODO(owner): evolve these in `advance`.
    angle_rad: f32,
    velocity_rad_s: f32,
    phase_current_a: [f32; 3],
    torque_nm: f32,
}

impl MotorModel {
    /// A motor named `name`, its shaft at `initial_angle_rad`, observing the default
    /// board firmware source ([`crate::SOURCE`]) with placeholder [`MotorParams`].
    pub fn new(name: &str, initial_angle_rad: f32) -> Self {
        Self {
            name: name.to_string(),
            bridge_source: SOURCE.to_string(),
            params: MotorParams::default(),
            angle_rad: initial_angle_rad,
            velocity_rad_s: 0.0,
            phase_current_a: [0.0; 3],
            torque_nm: 0.0,
        }
    }

    /// Override the placeholder parameters.
    pub fn with_params(mut self, params: MotorParams) -> Self {
        self.params = params;
        self
    }

    /// Observe the `bridge_source` firmware from a different instance name.
    pub fn with_bridge_source(mut self, source: &str) -> Self {
        self.bridge_source = source.to_string();
        self
    }

    fn out_id(&self, local: &str) -> SignalId {
        vsig_id(&self.name, local).expect("valid vsig id")
    }

    /// One bridge observation port's current value (`0.0` when never driven / not
    /// numeric — a dark bridge reads all zeros).
    fn observe(st: &StateTable, source: &str, local: &str) -> f32 {
        SignalId::new("vsig", source, local, None)
            .ok()
            .and_then(|id| st.current_value(&id).ok().flatten())
            .and_then(|v| v.as_f64())
            .unwrap_or(0.0) as f32
    }
}

impl Member for MotorModel {
    fn name(&self) -> &str {
        &self.name
    }

    fn advance(&mut self, _dt_us: u64, ctx: &mut MemberCtx) {
        // Inverter input (D6): the firmware's commanded bridge state this tick.
        let duty_u = Self::observe(ctx.st, &self.bridge_source, "PWM_U_duty");
        let duty_v = Self::observe(ctx.st, &self.bridge_source, "PWM_V_duty");
        let duty_w = Self::observe(ctx.st, &self.bridge_source, "PWM_W_duty");
        let enabled_u = Self::observe(ctx.st, &self.bridge_source, "PWM_U_enabled") != 0.0;
        let enabled_v = Self::observe(ctx.st, &self.bridge_source, "PWM_V_enabled") != 0.0;
        let enabled_w = Self::observe(ctx.st, &self.bridge_source, "PWM_W_enabled") != 0.0;
        let master_output_enabled = Self::observe(ctx.st, &self.bridge_source, "TIM1_MOE") != 0.0;

        // TODO(owner): electrical + mechanical dynamics.
        //   phase voltages = (enabled ? duty * vbus : float), master enable gates all;
        //   dI/dt from (v - R·I - BEMF(angle, velocity)) / L, sub-stepped if τ = L/R
        //   is short against dt_us; torque = kt · commutated current; then integrate
        //   velocity against inertia/friction and angle against velocity. The scaffold
        //   holds the state fixed so the outputs (and the encoder route) exist.
        let _inverter = (duty_u, duty_v, duty_w, enabled_u, enabled_v, enabled_w, master_output_enabled);
        let _params = &self.params;

        // Record the current state — the outputs exist from tick one, and `angle`
        // routes into the encoder model even while the dynamics are stubbed.
        let _ = ctx.st.record(&self.out_id("angle"), Value::F64(f64::from(self.angle_rad)));
        let _ = ctx.st.record(&self.out_id("velocity"), Value::F64(f64::from(self.velocity_rad_s)));
        let _ = ctx.st.record(&self.out_id("phase_current_u"), Value::F64(f64::from(self.phase_current_a[0])));
        let _ = ctx.st.record(&self.out_id("phase_current_v"), Value::F64(f64::from(self.phase_current_a[1])));
        let _ = ctx.st.record(&self.out_id("phase_current_w"), Value::F64(f64::from(self.phase_current_a[2])));
        let _ = ctx.st.record(&self.out_id("torque"), Value::F64(f64::from(self.torque_nm)));
    }

    fn set_enabled(&mut self, on: bool, st: &mut StateTable) {
        if on {
            let _ = st.register(self.out_id("angle"), Some("rad"));
            let _ = st.register(self.out_id("velocity"), Some("rad/s"));
            let _ = st.register(self.out_id("phase_current_u"), Some("A"));
            let _ = st.register(self.out_id("phase_current_v"), Some("A"));
            let _ = st.register(self.out_id("phase_current_w"), Some("A"));
            let _ = st.register(self.out_id("torque"), Some("Nm"));
        }
    }
}
