//! The DuplexTransfer primitive is engine-scoped and generic: two instantiation-side
//! model members couple over an SPI endpoint with no firmware in the transfer. A
//! zero-firmware [`Sil`] world hosts the engine.

use pcs_bldc_sil::Sil;
use voyant::{vsig_id, DuplexHandle, DuplexPeer, Member, MemberCtx, SignalId, StateTable, Value};

/// A duplex responder model — one struct, both roles: as a [`DuplexPeer`] it answers
/// each SPI transfer with its current 14-bit angle framed big-endian; as a [`Member`]
/// it advances that angle every tick and records it as a `vsig`.
struct DialResponder {
    name: String,
    angle: u16,
    step: u16,
}

impl DuplexPeer for DialResponder {
    fn transfer(&mut self, _tx: &[u8]) -> Vec<u8> {
        (self.angle & 0x3FFF).to_be_bytes().to_vec()
    }
}

impl Member for DialResponder {
    fn name(&self) -> &str {
        &self.name
    }
    fn advance(&mut self, _dt_us: u64, ctx: &mut MemberCtx) {
        self.angle = self.angle.wrapping_add(self.step);
        let id = vsig_id(&self.name, "angle").expect("valid vsig id");
        let _ = ctx.st.record(&id, Value::U32(u32::from(self.angle)));
    }
    fn set_enabled(&mut self, on: bool, st: &mut StateTable) {
        if on {
            let _ = st.register(vsig_id(&self.name, "angle").expect("valid vsig id"), None);
        }
    }
}

/// A duplex initiator model: initiates a READ-ANGLE transfer each advance over its
/// own endpoint and records the decoded angle as a `vsig`.
struct DialInitiator {
    name: String,
    handle: DuplexHandle,
}

impl Member for DialInitiator {
    fn name(&self) -> &str {
        &self.name
    }
    fn advance(&mut self, _dt_us: u64, ctx: &mut MemberCtx) {
        if let Some(rx) = ctx.duplex_transfer(self.handle, &[0xFF, 0xFF]) {
            let raw = (u16::from(rx[0]) << 8) | u16::from(rx[1]);
            let id = vsig_id(&self.name, "read_angle").expect("valid vsig id");
            let _ = ctx.st.record(&id, Value::U32(u32::from(raw & 0x3FFF)));
        }
    }
    fn set_enabled(&mut self, on: bool, st: &mut StateTable) {
        if on {
            let _ = st.register(vsig_id(&self.name, "read_angle").expect("valid vsig id"), None);
        }
    }
}

#[test]
fn model_to_model_duplex() {
    const ENDPOINT: &str = "spi:dial_initiator:cs";
    const STEP: u16 = 0x0100;

    let mut sim = Sil::new();
    // The responder is a shared member: added by value (idx 0), then linked to the bus
    // by its handle. It advances before the initiator (idx 1) reads each tick, so its
    // angle starts at 0x0000 and the initiator sees 0x0100, 0x0200, 0x0300.
    let responder = sim.add_member(DialResponder { name: "dial_responder".into(), angle: 0x0000, step: STEP });
    let handle = sim.link_duplex(ENDPOINT, responder).expect("link the model responder peer");
    sim.add_member(DialInitiator { name: "dial_initiator".into(), handle });

    for _ in 0..3 {
        sim.step().expect("engine step");
    }

    let read = sim
        .read("vsig:dial_initiator:read_angle")
        .ok()
        .flatten()
        .as_ref()
        .and_then(Value::as_u64);
    assert_eq!(
        read,
        Some(0x0300),
        "initiator reads the responder's frame synchronously (no firmware)"
    );

    // The engine force-recorded the exchange as :tx / :rx events under the endpoint
    // (one transfer/tick, never deduped -> three entries each).
    let tx_id = SignalId::parse(&format!("{ENDPOINT}:tx")).expect("valid spi id");
    let rx_id = SignalId::parse(&format!("{ENDPOINT}:rx")).expect("valid spi id");
    let n_tx = sim.state().changes(&tx_id).map(|c| c.len()).unwrap_or(0);
    let n_rx = sim.state().changes(&rx_id).map(|c| c.len()).unwrap_or(0);
    let last_rx = sim.state().current_value(&rx_id).ok().flatten();
    assert!(
        (n_tx == 3) && (n_rx == 3) && (last_rx == Some(Value::Bytes(vec![0x03, 0x00]))),
        "engine records model duplex as :tx/:rx events: {n_tx} tx + {n_rx} rx; last rx = {last_rx:?}"
    );
}
