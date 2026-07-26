//! AS5048 encoder model scaffolding — one struct, both roles: a [`Member`] that
//! consumes commanded-angle inputs and publishes outputs, and a [`DuplexPeer`]
//! that answers the firmware's SPI reads. Wire two instances:
//!
//! ```text
//! let motor = eng.add_member(As5048Model::new("as5048_motor"));
//! eng.link_duplex("spi:pcs_bldc:AS5048_1", motor)?;
//! let dial = eng.add_member(As5048Model::new("dial"));
//! eng.link_duplex("spi:pcs_bldc:AS5048_2", dial)?;
//! ```
//!
//! Device contract: response frame = bit15 even parity, bit14 error flag,
//! bits[13:0] angle (16384 counts/rev), big-endian on the wire; the firmware
//! polls with READ-ANGLE `0xFFFF`, two pipelined transfers per 1 ms tick.

use voyant::{vsig_id, DuplexPeer, Member, MemberCtx, SignalId, StateTable};

/// An AS5048 magnetic encoder: commanded via `vsig:<name>:angle_deg` /
/// `angle_rad` (last write wins), read out by the firmware over SPI.
pub struct As5048Model {
    name: String,
    // TODO(owner): model state — current angle, command/response pipeline, ...
}

impl As5048Model {
    pub fn new(name: &str) -> Self {
        Self {
            name: name.to_string(),
        }
    }

    fn angle_deg_id(&self) -> SignalId {
        vsig_id(&self.name, "angle_deg").expect("valid vsig id")
    }
    fn angle_rad_id(&self) -> SignalId {
        vsig_id(&self.name, "angle_rad").expect("valid vsig id")
    }
    fn raw_id(&self) -> SignalId {
        vsig_id(&self.name, "raw_encoder_ticks").expect("valid vsig id")
    }
}

impl Member for As5048Model {
    fn name(&self) -> &str {
        &self.name
    }

    fn advance(&mut self, _dt_us: u64, ctx: &mut MemberCtx) {
        // Commanded-angle inputs, either unit (a test writes whichever it likes).
        let deg = ctx.st.current_value(&self.angle_deg_id()).ok().flatten();
        let rad = ctx.st.current_value(&self.angle_rad_id()).ok().flatten();
        let _ = (deg, rad);
        // TODO(owner): fold the freshest input into the model's angle and record
        // the quantized output: ctx.st.record(&self.raw_id(), Value::U32(...)).
    }

    fn set_enabled(&mut self, on: bool, st: &mut StateTable) {
        if on {
            let _ = st.register(self.angle_deg_id(), Some("deg"));
            let _ = st.register(self.angle_rad_id(), Some("rad"));
            let _ = st.register(self.raw_id(), Some("counts"));
        }
    }
}

impl DuplexPeer for As5048Model {
    fn transfer(&mut self, _tx: &[u8]) -> Vec<u8> {
        // TODO(owner): parse the command frame, model the one-frame pipeline,
        // frame the angle with parity/error bits. Until then: all-ones, which
        // decodes as an error frame (a floating bus), never a silently-valid
        // angle 0.
        vec![0xFF, 0xFF]
    }
}
