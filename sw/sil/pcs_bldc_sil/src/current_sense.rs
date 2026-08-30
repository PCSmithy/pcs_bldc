//! Current-sense front end — the shunt + amplifier chain from plant currents to ADC
//! pin volts.
//!
//! All ports live in the model's own namespace; the sim wiring routes plant currents
//! in and ADC pin voltages out (see [`crate::wiring::wire_current_sense`]). Inputs:
//! `vsig:<name>:{i_u,i_v,i_w,i_bus}` (A). Outputs: `vsig:<name>:{i_u_vsense,i_v_vsense,i_w_vsense,i_bus_vsense}`
//! (V). Transfer per channel: `v = clamp(bias + gain · i, 0, vref)` — the clamp is
//! the amplifier rails, so fault-level currents saturate at full scale and negative
//! bus current (regen) reads 0 V, matching the board's ground-referenced INA180.

use voyant::{vsig_id, Cadence, Member, MemberCtx, SigHandle, SignalId, StateTable, Value};

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
    // Pre-resolved handles (resolve-once), filled at enable: 4 inputs, 4 outputs.
    hin: [Option<SigHandle>; 4],
    hout: [Option<SigHandle>; 4],
}

impl CurrentSenseModel {
    pub fn new(name: &str) -> Self {
        Self {
            name: name.to_string(),
            params: CurrentSenseParams::default(),
            hin: [None; 4],
            hout: [None; 4],
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

    /// One input port's current value by handle (`0.0` when never driven).
    fn observe(st: &StateTable, h: Option<SigHandle>) -> f64 {
        h.and_then(|h| st.current_f64(h)).unwrap_or(0.0)
    }

    /// Register one port and capture its handle.
    fn reg(st: &mut StateTable, id: SignalId, unit: Option<&str>) -> Option<SigHandle> {
        let _ = st.register(id.clone(), unit);
        st.handle(&id)
    }
}

impl Member for CurrentSenseModel {
    fn name(&self) -> &str {
        &self.name
    }

    /// A pure transform: re-evaluate only when a routed current actually changed.
    fn cadence(&self) -> Cadence {
        Cadence::OnInputChange
    }

    fn advance(&mut self, _dt_us: u64, ctx: &mut MemberCtx) {
        let i_k = [
            Self::observe(ctx.st, self.hin[0]),
            Self::observe(ctx.st, self.hin[1]),
            Self::observe(ctx.st, self.hin[2]),
        ];
        let i_bus = Self::observe(ctx.st, self.hin[3]);

        let mut i_k_vsense = [self.params.phase_bias_v; 3];

        for k in 0..N_PHASES {
            i_k_vsense[k] = (self.params.phase_bias_v + self.params.phase_gain_v_per_a * i_k[k])
                .clamp(0.0, self.params.vref_v);
        }
        let i_bus_vsense = (self.params.bus_bias_v + i_bus * self.params.bus_gain_v_per_a)
            .clamp(0.0, self.params.vref_v);

        for (h, v) in self.hout.iter().zip([
            i_k_vsense[0],
            i_k_vsense[1],
            i_k_vsense[2],
            i_bus_vsense,
        ]) {
            if let Some(h) = h {
                let _ = ctx.st.record_by(*h, Value::F64(v));
            }
        }
    }

    fn set_enabled(&mut self, on: bool, st: &mut StateTable) {
        if on {
            for (i, c) in ["i_u", "i_v", "i_w", "i_bus"].iter().enumerate() {
                self.hin[i] = Self::reg(st, self.port_id(c), Some("A"));
            }
            for (i, c) in ["i_u_vsense", "i_v_vsense", "i_w_vsense", "i_bus_vsense"]
                .iter()
                .enumerate()
            {
                self.hout[i] = Self::reg(st, self.port_id(c), Some("V"));
            }
        }
    }
}
