//! The link throughput test over the framed protocol, end to end: a
//! LinkTestRequest injected through the sim USB RX path is accepted, and the
//! board streams exactly the requested frames with consecutive sequence
//! numbers and the patterned payload the host verifies content against.
//!
//! The sim USB TX buffer never drains on its own, so the capture is read and
//! zeroed every step — the same trick trace_stream.rs uses, here feeding one
//! incremental Deframer so a frame split across that boundary still rejoins.
//!
//! Envelopes are hand-encoded/decoded here (minimal varint helpers), matching
//! trace_stream.rs.

use pcs_bldc_sil::wire::{frame, Deframer};
use pcs_bldc_sil::{cid, Sil, SOURCE};

/// The measurement this scenario asks for.
const PAYLOAD_BYTES: u32 = 64;
const FRAME_COUNT: u32 = 200;

/// Step budget for the stream. The sim transport holds 2 KB, so a step carries
/// ~26 frames of this size; the margin absorbs the telemetry sharing the link.
const MAX_STEPS: u32 = 120;

// --- minimal protobuf helpers (proto3 varint + length-delimited fields) ---

fn put_varint(mut v: u64, out: &mut Vec<u8>) {
    loop {
        let byte = (v & 0x7F) as u8;
        v >>= 7;
        if v != 0 {
            out.push(byte | 0x80);
        } else {
            out.push(byte);
            break;
        }
    }
}

fn get_varint(bytes: &[u8], i: &mut usize) -> Option<u64> {
    let mut v = 0u64;
    let mut shift = 0u32;
    loop {
        let byte = *bytes.get(*i)?;
        *i += 1;
        v |= u64::from(byte & 0x7F) << shift;
        if byte & 0x80 == 0 {
            return Some(v);
        }
        shift += 7;
    }
}

/// Field key for a length-delimited (wire type 2) field.
fn put_len_key(field: u32, out: &mut Vec<u8>) {
    put_varint(u64::from((field << 3) | 2), out);
}

fn envelope(request_id: u64, payload_field: u32, payload: &[u8]) -> Vec<u8> {
    let mut env = Vec::new();
    if request_id != 0 {
        env.push(0x08);
        put_varint(request_id, &mut env);
    }
    put_len_key(payload_field, &mut env);
    put_varint(payload.len() as u64, &mut env);
    env.extend_from_slice(payload);
    env
}

/// Parse an Envelope payload into (request_id, payload field number, bytes).
fn parse_envelope(payload: &[u8]) -> Option<(u64, u32, Vec<u8>)> {
    let mut i = 0usize;
    let mut request_id = 0u64;
    let mut key = get_varint(payload, &mut i)?;
    if key == 0x08 {
        request_id = get_varint(payload, &mut i)?;
        key = get_varint(payload, &mut i)?;
    }
    if key & 7 != 2 {
        return None;
    }
    let field = (key >> 3) as u32;
    let len = get_varint(payload, &mut i)? as usize;
    let bytes = payload.get(i..i + len)?.to_vec();
    Some((request_id, field, bytes))
}

/// Collect a submessage's scalar varint and bytes fields by field number.
fn parse_fields(msg: &[u8]) -> Vec<(u32, u64, Vec<u8>)> {
    let mut out = Vec::new();
    let mut i = 0usize;
    while i < msg.len() {
        let Some(key) = get_varint(msg, &mut i) else { break };
        let field = (key >> 3) as u32;
        match key & 7 {
            0 => {
                let Some(v) = get_varint(msg, &mut i) else { break };
                out.push((field, v, Vec::new()));
            }
            2 => {
                let Some(len) = get_varint(msg, &mut i) else { break };
                let Some(bytes) = msg.get(i..i + len as usize) else { break };
                i += len as usize;
                out.push((field, 0, bytes.to_vec()));
            }
            _ => break,
        }
    }
    out
}

fn field_varint(fields: &[(u32, u64, Vec<u8>)], field: u32) -> u64 {
    fields
        .iter()
        .find(|(f, _, _)| *f == field)
        .map(|(_, v, _)| *v)
        .unwrap_or(0)
}

fn field_bytes(fields: &[(u32, u64, Vec<u8>)], field: u32) -> &[u8] {
    fields
        .iter()
        .find(|(f, _, _)| *f == field)
        .map(|(_, _, b)| b.as_slice())
        .unwrap_or(&[])
}

// --- request builder (shared.Envelope oneof field numbers) ---

const F_RESPONSE: u32 = 3;
const F_LINK_TEST_REQUEST: u32 = 7;
const F_LINK_TEST_FRAME: u32 = 8;

fn link_test_request(request_id: u64, payload_bytes: u32, frame_count: u32) -> Vec<u8> {
    let mut msg = Vec::new();
    msg.push(0x08);
    put_varint(u64::from(payload_bytes), &mut msg);
    msg.push(0x10);
    put_varint(u64::from(frame_count), &mut msg);
    envelope(request_id, F_LINK_TEST_REQUEST, &msg)
}

// --- sim USB RX injection ---

/// How many rx[] elements are registered for command writes (must cover the
/// longest injected frame).
const RX_REG: usize = 64;

fn inject(sim: &mut Sil, envelope_bytes: &[u8]) {
    let wire = frame(envelope_bytes);
    assert!(
        wire.len() <= RX_REG,
        "injected frame ({} B) exceeds the {} registered rx elements",
        wire.len(),
        RX_REG
    );
    for (i, &byte) in wire.iter().enumerate() {
        sim.write(&cid(&format!("HW_USB_sim_data.rx[{i}]")), u32::from(byte))
            .expect("write rx byte");
    }
    sim.write(&cid("HW_USB_sim_data.rxHead"), 0u32)
        .expect("write rxHead");
    sim.write(&cid("HW_USB_sim_data.rxTail"), wire.len() as u32)
        .expect("write rxTail");
}

/// Read the sim USB TX capture and zero it, so the next step sees a drained
/// transport — the host end of the link the board is measuring against.
fn drain_tx(sim: &mut Sil) -> Vec<u8> {
    let fw = sim.fw();
    let len = fw.read_cvar("HW_USB_sim_data.txLen").as_u64().unwrap_or(0);
    let mut bytes = Vec::with_capacity(len as usize);
    for i in 0..len {
        bytes.push(
            fw.read_cvar(&format!("HW_USB_sim_data.tx[{i}]"))
                .as_u64()
                .unwrap_or(0) as u8,
        );
    }
    drop(fw);
    sim.write(&cid("HW_USB_sim_data.txLen"), 0u32)
        .expect("drain tx capture");
    bytes
}

// [test->sys~conn_004~1]
// [test->fw~conn_server_005~1]
#[test]
fn link_test() {
    let mut sim = Sil::new();
    let mut fwm = sim.load_firmware(SOURCE);
    for i in 0..RX_REG {
        fwm.register_cvar_in_state_table(&format!("HW_USB_sim_data.rx[{i}]"));
    }
    sim.add_member(fwm);

    sim.write(&cid("HW_USB_sim_data.connected"), true)
        .expect("write connected");
    for _ in 0..3 {
        sim.step().expect("engine step");
    }
    let _ = drain_tx(&mut sim);

    inject(&mut sim, &link_test_request(1, PAYLOAD_BYTES, FRAME_COUNT));

    let mut deframer = Deframer::new();
    let mut envelopes: Vec<(u64, u32, Vec<u8>)> = Vec::new();
    let mut frames = 0u32;
    for _ in 0..MAX_STEPS {
        sim.step().expect("engine step");
        let capture = drain_tx(&mut sim);
        for payload in deframer.push(&capture) {
            if let Some(env) = parse_envelope(&payload) {
                if env.1 == F_LINK_TEST_FRAME {
                    frames += 1;
                }
                envelopes.push(env);
            }
        }
        if frames >= FRAME_COUNT {
            break;
        }
    }

    // The request is accepted before a single frame streams.
    let response = envelopes
        .iter()
        .find(|(id, field, _)| (*id == 1) && (*field == F_RESPONSE))
        .map(|(_, _, bytes)| parse_fields(bytes))
        .expect("Response to the LinkTestRequest");
    assert_eq!(field_varint(&response, 1), 1, "link test accepted");

    // Exactly the requested frames, seq 0..FRAME_COUNT-1, each carrying the
    // (seq + i) & 0xFF pattern — a host verifies content without a side channel.
    let streamed: Vec<_> = envelopes
        .iter()
        .filter(|(_, field, _)| *field == F_LINK_TEST_FRAME)
        .collect();
    assert_eq!(
        streamed.len() as u32,
        FRAME_COUNT,
        "exactly the requested frame count streams"
    );
    for (i, (request_id, _, bytes)) in streamed.iter().enumerate() {
        assert_eq!(*request_id, 0, "frames stream unsolicited, like Samples");
        let fields = parse_fields(bytes);
        let seq = field_varint(&fields, 1);
        assert_eq!(seq, i as u64, "consecutive sequence numbers from zero");
        let payload = field_bytes(&fields, 2);
        assert_eq!(payload.len() as u32, PAYLOAD_BYTES, "requested payload size");
        let expected: Vec<u8> = (0..PAYLOAD_BYTES)
            .map(|i| ((seq as u32).wrapping_add(i) & 0xFF) as u8)
            .collect();
        assert_eq!(payload, expected.as_slice(), "seq-derived payload pattern");
    }

    // The count is reached, so the service is free: nothing more streams.
    for _ in 0..5 {
        sim.step().expect("engine step");
        let capture = drain_tx(&mut sim);
        for payload in deframer.push(&capture) {
            if let Some((_, field, _)) = parse_envelope(&payload) {
                assert_ne!(field, F_LINK_TEST_FRAME, "the stream stops at the count");
            }
        }
    }
}
