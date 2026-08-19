//! The signal-trace services over the framed protocol, end to end: a
//! WatchRequest installed through the sim USB RX path streams the sim trace
//! window's 1 kHz counter back as coherent, consecutive Samples; the trace
//! capability report carries the board's budgets; a written span reads back.
//!
//! Envelopes are hand-encoded/decoded here (minimal varint helpers) — the
//! full protobuf codec arrives with the desktop-app work.

use pcs_bldc_sil::wire::{frame, parse_frames, read_tx_capture};
use pcs_bldc_sil::{cid, Sil, SOURCE};

/// Protocol address of the sim trace window (`app_server_simTraceWindow32`);
/// word [0] is the 1 kHz counter task_1ms increments.
const WINDOW_BASE: u32 = 0x2000_0000;

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

fn field_bytes<'a>(fields: &'a [(u32, u64, Vec<u8>)], field: u32) -> &'a [u8] {
    fields
        .iter()
        .find(|(f, _, _)| *f == field)
        .map(|(_, _, b)| b.as_slice())
        .unwrap_or(&[])
}

// --- request builders (shared.Envelope oneof field numbers) ---

const F_WATCH_REQUEST: u32 = 30;
const F_TRACE_STATUS: u32 = 31;
const F_SAMPLES: u32 = 33;
const F_READ_REQUEST: u32 = 34;
const F_READ_REPLY: u32 = 35;
const F_WRITE_REQUEST: u32 = 36;
const F_RESPONSE: u32 = 3;

fn watch_request(request_id: u64, watches: &[(u32, u32, u32)]) -> Vec<u8> {
    let mut wr = Vec::new();
    for &(address, size, period_ms) in watches {
        let mut w = Vec::new();
        w.push(0x08);
        put_varint(u64::from(address), &mut w);
        w.push(0x10);
        put_varint(u64::from(size), &mut w);
        w.push(0x18);
        put_varint(u64::from(period_ms), &mut w);
        wr.push(0x0A);
        put_varint(w.len() as u64, &mut wr);
        wr.extend_from_slice(&w);
    }
    envelope(request_id, F_WATCH_REQUEST, &wr)
}

fn write_request(request_id: u64, address: u32, data: &[u8]) -> Vec<u8> {
    let mut msg = Vec::new();
    msg.push(0x08);
    put_varint(u64::from(address), &mut msg);
    msg.push(0x12);
    put_varint(data.len() as u64, &mut msg);
    msg.extend_from_slice(data);
    envelope(request_id, F_WRITE_REQUEST, &msg)
}

fn read_request(request_id: u64, address: u32, size: u32) -> Vec<u8> {
    let mut msg = Vec::new();
    msg.push(0x08);
    put_varint(u64::from(address), &mut msg);
    msg.push(0x10);
    put_varint(u64::from(size), &mut msg);
    envelope(request_id, F_READ_REQUEST, &msg)
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

// [test->sys~obs_005~1]
// [test->sys~obs_008~1]
// [test->sys~obs_009~1]
// [test->fw~conn_trace_002~1]
// [test->fw~conn_trace_004~1]
// [test->fw~conn_trace_006~1]
// [test->fw~conn_trace_007~1]
#[test]
fn trace_stream() {
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
    sim.write(&cid("HW_USB_sim_data.txLen"), 0u32)
        .expect("drain greeting-era capture");

    // Install one watch on the 1 kHz counter word.
    inject(&mut sim, &watch_request(1, &[(WINDOW_BASE, 4, 1)]));
    for _ in 0..30 {
        sim.step().expect("engine step");
    }

    let frames = parse_frames(&read_tx_capture(&sim.fw()));
    let envelopes: Vec<_> = frames.iter().filter_map(|f| parse_envelope(f)).collect();

    // The accepted WatchRequest answers with the trace capability report,
    // carrying the board's configured budgets and the list's usage
    // (r = 4 B x 1000 Hz + 21 B x 1000 Hz).
    let status = envelopes
        .iter()
        .find(|(id, field, _)| (*id == 1) && (*field == F_TRACE_STATUS))
        .map(|(_, _, bytes)| parse_fields(bytes))
        .expect("TraceStatus reply to the accepted WatchRequest");
    assert_eq!(field_varint(&status, 1), 2048, "sample-RAM budget");
    assert_eq!(field_varint(&status, 2), 4 + 4, "worst-tick RAM usage");
    assert_eq!(field_varint(&status, 3), 1_100_000, "link budget");
    assert_eq!(field_varint(&status, 4), 25_000, "link rate");

    // The counter arrives as coherent, consecutive Samples: tick counts
    // consecutive from zero, the watched value advancing in lockstep.
    let samples: Vec<(u64, u32)> = envelopes
        .iter()
        .filter(|(_, field, _)| *field == F_SAMPLES)
        .map(|(_, _, bytes)| {
            let fields = parse_fields(bytes);
            let tick = field_varint(&fields, 1);
            let data = field_bytes(&fields, 2);
            assert_eq!(data.len(), 4, "one 4-byte watch per tick");
            let value = u32::from_le_bytes([data[0], data[1], data[2], data[3]]);
            (tick, value)
        })
        .collect();
    assert!(
        samples.len() >= 20,
        "a 1 ms watch streams every tick: {} Samples over 30 ticks",
        samples.len()
    );
    let (t0, v0) = samples[0];
    assert_eq!(t0, 0, "the stream restarts at tick zero on install");
    for (i, &(tick, value)) in samples.iter().enumerate() {
        assert_eq!(tick, t0 + i as u64, "consecutive tick counts");
        assert_eq!(
            u64::from(value),
            u64::from(v0) + i as u64,
            "counter values consecutive and coherent with the tick count"
        );
    }

    // One-shot write then read back: the span's current contents round-trip.
    sim.write(&cid("HW_USB_sim_data.txLen"), 0u32)
        .expect("drain capture");
    inject(&mut sim, &write_request(2, WINDOW_BASE + 8, &[0xDE, 0xAD, 0xBE, 0xEF]));
    for _ in 0..3 {
        sim.step().expect("engine step");
    }
    inject(&mut sim, &read_request(3, WINDOW_BASE + 8, 4));
    for _ in 0..3 {
        sim.step().expect("engine step");
    }

    let frames = parse_frames(&read_tx_capture(&sim.fw()));
    let envelopes: Vec<_> = frames.iter().filter_map(|f| parse_envelope(f)).collect();
    let write_reply = envelopes
        .iter()
        .find(|(id, field, _)| (*id == 2) && (*field == F_RESPONSE))
        .map(|(_, _, bytes)| parse_fields(bytes))
        .expect("Response to the WriteRequest");
    assert_eq!(field_varint(&write_reply, 1), 1, "write accepted");
    let read_reply = envelopes
        .iter()
        .find(|(id, field, _)| (*id == 3) && (*field == F_READ_REPLY))
        .map(|(_, _, bytes)| parse_fields(bytes))
        .expect("ReadReply to the ReadRequest");
    assert_eq!(
        field_bytes(&read_reply, 1),
        &[0xDE, 0xAD, 0xBE, 0xEF],
        "the read returns the span's current contents"
    );
}
