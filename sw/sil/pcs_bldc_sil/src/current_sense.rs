//! Current-sense front end — the shunt + amplifier chain from plant currents to ADC
//! pin volts.
//!
//! All ports live in the model's own namespace; the sim wiring routes plant currents
//! in and ADC pin voltages out (see [`crate::wiring::wire_current_sense`]). Inputs:
//! `vsig:<name>:{i_u,i_v,i_w,i_bus}` (A). Outputs: `vsig:<name>:{i_u_vsense,i_v_vsense,i_w_vsense,i_bus_vsense}`
//! (V). Transfer per channel: `v = clamp(bias + gain · i, 0, vref)` — the clamp is
//! the amplifier rails, so fault-level currents saturate at full scale and negative
//! bus current (regen) reads 0 V, matching the board's ground-referenced INA180.

use voyant::{vsig_id, Member, MemberCtx, SignalId, StateTable, Value};

const N_PHASES: usize = 3;

/// Sense-chain parameters. Defaults mirror the board front end declared in
/// `sw/fw/src/io/bridge/IO_bridge_channels.c` — the firmware's decode constants and
/// this model must describe the same hardware.
#[derive(Clone, Copy, Debug)]
pub struct CurrentSenseParams {
    /// Phase zero-current midpoint (V): VREF/2 bias for bipolar sensing.
    pub phase_bias_v: f64,
    /// Phase transfer slope (V/A): INA240A3 (100 V/V) across a 1 mΩ shunt.
    pub phase_gain_v_per_a: f64,
    /// Bus zero-current level (V): ground-referenced, no bias.
    pub bus_bias_v: f64,
    /// Bus transfer slope (V/A): INA180A2 (50 V/V) across a 12 mΩ shunt.
    pub bus_gain_v_per_a: f64,
    /// Amplifier rail (V): outputs clamp to `[0, vref_v]`.
    pub vref_v: f64,
}

impl Default for CurrentSenseParams {
    fn default() -> Self {
        Self {
            phase_bias_v: 1.65,
            phase_gain_v_per_a: 0.1,
            bus_bias_v: 0.0,
            bus_gain_v_per_a: 0.6,
            vref_v: 3.3,
        }
    }
}

/// The board's current-sense chain as a model member: reads the plant currents from
/// its input ports each tick and records the ADC pin voltages on its output ports.
pub struct CurrentSenseModel {
    name: String,
    params: CurrentSenseParams,
}

impl CurrentSenseModel {
    pub fn new(name: &str) -> Self {
        Self {
            name: name.to_string(),
            params: CurrentSenseParams::default(),
        }
    }

    /// Override the board-default parameters.
    pub fn with_params(mut self, params: CurrentSenseParams) -> Self {
        self.params = params;
        self
    }

    fn port_id(&self, local: &str) -> SignalId {
        vsig_id(&self.name, local).expect("valid vsig id")
    }

    /// One input port's current value (`0.0` when never driven).
    fn observe(st: &StateTable, source: &str, local: &str) -> f64 {
        SignalId::new("vsig", source, local, None)
            .ok()
            .and_then(|id| st.current_value(&id).ok().flatten())
            .and_then(|v| v.as_f64())
            .unwrap_or(0.0)
    }
}

impl Member for CurrentSenseModel {
    fn name(&self) -> &str {
        &self.name
    }

    fn advance(&mut self, _dt_us: u64, ctx: &mut MemberCtx) {
        let ob = |local: &str| Self::observe(ctx.st, &self.name, local);
        let i_k = [ob("i_u"), ob("i_v"), ob("i_w")];
        let i_bus = ob("i_bus");

        let mut i_k_vsense = [self.params.phase_bias_v; 3];

        for k in 0..N_PHASES {
            i_k_vsense[k] = (self.params.phase_bias_v + self.params.phase_gain_v_per_a * i_k[k])
                .clamp(0.0, self.params.vref_v);
        }
        let i_bus_vsense = (self.params.bus_bias_v + i_bus * self.params.bus_gain_v_per_a)
            .clamp(0.0, self.params.vref_v);

        for (c, v) in ["i_u_vsense", "i_v_vsense", "i_w_vsense"]
            .iter()
            .zip(i_k_vsense)
        {
            let _ = ctx.st.record(&self.port_id(c), Value::F64(v));
        }
        let _ = ctx
            .st
            .record(&self.port_id("i_bus_vsense"), Value::F64(i_bus_vsense));
    }

    fn set_enabled(&mut self, on: bool, st: &mut StateTable) {
        if on {
            // inputs
            let _ = st.register(self.port_id("i_u"), Some("A"));
            let _ = st.register(self.port_id("i_v"), Some("A"));
            let _ = st.register(self.port_id("i_w"), Some("A"));
            let _ = st.register(self.port_id("i_bus"), Some("A"));

            // outputs
            let _ = st.register(self.port_id("i_u_vsense"), Some("V"));
            let _ = st.register(self.port_id("i_v_vsense"), Some("V"));
            let _ = st.register(self.port_id("i_w_vsense"), Some("V"));
            let _ = st.register(self.port_id("i_bus_vsense"), Some("V"));
        }
    }
}
