//! AS5048 magnetic encoder model — one struct, both roles: a [`Member`] whose
//! `vsig:<name>:angle` (canonical rad) is the commanded shaft angle, and a
//! [`DuplexPeer`] answering the firmware's SPI reads. Wire two instances:
//!
//! ```text
//! let motor = eng.add_member(As5048Model::new("as5048_motor", 0.0));
//! eng.link_duplex("spi:pcs_bldc:AS5048_1", motor)?;
//! let dial = eng.add_member(As5048Model::new("dial", 0.0));
//! eng.link_duplex("spi:pcs_bldc:AS5048_2", dial)?;
//! ```
//!
//! Device contract: frames are big-endian on the wire; a response carries
//! bit15 even parity, bit14 error flag, bits[13:0] angle (16384 counts/rev).
//! The response to command N arrives in transfer N+1 (one-frame pipeline);
//! the firmware polls READ-ANGLE `0xFFFF`, two pipelined transfers per tick.
//!
//! The model is [`Cadence::OnDemand`] — never scheduled; each transfer samples
//! the commanded angle at the transaction instant, as the silicon does
//! (`docs/sil/member-cadence.md`).

use prng::Prng;
use voyant::{
    vsig_id, Cadence, DuplexPeer, Member, MemberCtx, SigHandle, SignalId, StateTable, Value,
};

const ANGLE_RESOLUTION_TICKS_PER_REV: u64 = 16384;
const ANGLE_RESOLUTION_TICKS_PER_REV_F32: f32 = ANGLE_RESOLUTION_TICKS_PER_REV as f32;
const TWO_PI: f32 = 2.0 * std::f32::consts::PI;
const SPI_COMMAND_LEN: usize = 2;

const REG_ADDR_ANGLE: u16 = 0x3FFF;

fn get_lsb(angle_rad: f32) -> f32 {
    angle_rad * ANGLE_RESOLUTION_TICKS_PER_REV_F32 / TWO_PI
}

/// Unpack a wire frame (big-endian, byte 0 = bits 15..8) into `(parity_ok, error_flag,
/// raw14)`. Even parity: the ones across all 16 bits, the parity bit included, are even.
/// `None` for anything that is not a 2-byte frame.
pub fn decode_frame(frame: &[u8]) -> Option<(bool, bool, u16)> {
    (frame.len() == SPI_COMMAND_LEN).then(|| {
        let f = u16::from_be_bytes([frame[0], frame[1]]);
        (
            f.count_ones().is_multiple_of(2),
            (f & 0x4000) != 0,
            f & 0x3FFF,
        )
    })
}

/// An AS5048 encoder instance. Registers `angle` (in, canonical rad; unit asks
/// convert at the table boundary) and `raw_encoder_ticks` (out, quantized).
pub struct As5048Model {
    name: String,

    // Pre-resolved handles (resolve-once), filled at enable.
    h_angle: Option<SigHandle>,
    h_raw: Option<SigHandle>,

    current_angle_lsb: f32,
    current_angle_rad: f32,
    current_angle_raw: u16,

    spi_error: bool,
    /// The pipelined response armed by the previous command: 14-bit data
    /// (parity is applied at emission). Power-on sentinel `0xFFFF` reads as a
    /// parity-valid error frame — a bus nobody has commanded yet.
    response_frame: u16,

    sigma_lsb: f32,
    rng: Option<Prng>,
}

impl As5048Model {
    pub fn new(name: &str, current_angle_rad: f32) -> Self {
        Self {
            name: name.to_string(),
            h_angle: None,
            h_raw: None,
            current_angle_lsb: get_lsb(current_angle_rad),
            current_angle_rad,
            current_angle_raw: 0,
            spi_error: false,
            response_frame: 0xFFFF,
            sigma_lsb: 0.0,
            rng: None,
        }
    }

    pub fn with_noise(mut self, sigma_lsb: f32, seed: u64) -> Self {
        self.sigma_lsb = sigma_lsb;
        self.rng = Some(Prng::new(seed));

        self
    }

    fn angle_id(&self) -> SignalId {
        vsig_id(&self.name, "angle").expect("valid vsig id")
    }
    fn raw_id(&self) -> SignalId {
        vsig_id(&self.name, "raw_encoder_ticks").expect("valid vsig id")
    }

    /// Sample the commanded angle at the transaction instant: fold into [0, 2π),
    /// quantize, publish the folded angle + noise-free `raw_encoder_ticks`.
    fn sample(&mut self, ctx: &mut MemberCtx) {
        if let Some(angle_rad) = self.h_angle.and_then(|h| ctx.st.current_f64(h)) {
            self.current_angle_rad = angle_rad as f32;
        }
        self.current_angle_rad = self.current_angle_rad.rem_euclid(TWO_PI);
        self.current_angle_lsb = get_lsb(self.current_angle_rad);
        self.current_angle_raw = (self.current_angle_rad * (ANGLE_RESOLUTION_TICKS_PER_REV as f32)
            / TWO_PI)
            .round() as u16;
        if let Some(h) = self.h_raw {
            let _ = ctx.st.record_by(h, Value::U32(self.current_angle_raw as u32));
        }
        if let Some(h) = self.h_angle {
            let _ = ctx
                .st
                .record_by(h, Value::F64(f64::from(self.current_angle_rad)));
        }
    }
}

impl Member for As5048Model {
    fn name(&self) -> &str {
        &self.name
    }

    fn cadence(&self) -> Cadence {
        Cadence::OnDemand
    }

    fn advance(&mut self, _dt_us: u64, _ctx: &mut MemberCtx) {
        // OnDemand: never scheduled — the bus drives everything (transfer).
    }

    fn set_enabled(&mut self, on: bool, st: &mut StateTable) {
        if on {
            let _ = st.register(self.angle_id(), Some("rad"));
            let _ = st.register(self.raw_id(), Some("counts"));
            self.h_angle = st.handle(&self.angle_id());
            self.h_raw = st.handle(&self.raw_id());
        }
    }
}

impl DuplexPeer for As5048Model {
    fn transfer(&mut self, tx: &[u8], ctx: &mut MemberCtx) -> Vec<u8> {
        self.sample(ctx);

        // Emit the response armed by the PREVIOUS command (one-frame pipeline).
        let mut resp_frame: u16 = match self.spi_error {
            true => {
                self.spi_error = false; // TODO: clear only via a CLEAR-ERROR-FLAG (0x0001) read
                0x4000 // error flag
            }
            false => self.response_frame,
        };

        // Process this command — arms the NEXT response.
        let valid_len = tx.len() == SPI_COMMAND_LEN;
        if valid_len {
            let command_frame = u16::from_be_bytes([tx[0], tx[1]]);
            if command_frame.count_ones().is_multiple_of(2) {
                let read = (command_frame & 0x4000) != 0;
                let addr = command_frame & 0x3FFF;

                if read && (addr == REG_ADDR_ANGLE) {
                    // apply noise (if enabled) to lsb angle
                    let mut measurement_noise: f32 = 0.0;
                    if self.sigma_lsb > 0.0 {
                        if let Some(rng) = self.rng.as_mut() {
                            measurement_noise = rng.next_gauss() as f32 * self.sigma_lsb;
                        }
                    }

                    let noisy = self.current_angle_lsb + measurement_noise;
                    self.response_frame =
                        noisy.round().rem_euclid(ANGLE_RESOLUTION_TICKS_PER_REV_F32) as u16
                            & 0x3FFF;
                }

                if !read {
                    panic!("Writes not implemented yet!")
                }
            } else {
                self.spi_error = true
            }
        } else {
            self.spi_error = true
        }

        // Even parity over all 16 bits, applied to every outgoing frame.
        if !resp_frame.count_ones().is_multiple_of(2) {
            resp_frame |= 0x8000;
        }
        resp_frame.to_be_bytes().to_vec()
    }
}
