//! Wire codec for the board's framed protocol (fw~conn_proto_002):
//! COBS-delimited frames carrying an Envelope followed by its CRC-32.
//! Shared by the SIL harness (whole-buffer capture parsing) and the desktop
//! app (live streaming). The hand-rolled CRC mirrors the board's `lib_crc32`
//! and `tools/pcs_client.py` — the reference-vector test is the
//! cross-language contract; keep all three in step.

/// Leading wire bytes of an Envelope whose payload is board.Telemetry
/// (shared.proto field 62, length-delimited: varint tag 0xF2 0x03). An
/// envelope with request_id 0 encodes starting at its oneof tag.
pub const TELEMETRY_TAG_BYTES: [u8; 2] = [0xF2, 0x03];

/// Decode one COBS block (delimiters already stripped); `None` on malformed
/// input.
pub fn cobs_decode(seg: &[u8]) -> Option<Vec<u8>> {
    let mut out = Vec::with_capacity(seg.len());
    let mut i = 0usize;
    while i < seg.len() {
        let code = seg[i] as usize;
        if code == 0 || i + code > seg.len() {
            return None;
        }
        out.extend_from_slice(&seg[i + 1..i + code]);
        i += code;
        if code != 0xFF && i < seg.len() {
            out.push(0);
        }
    }
    Some(out)
}

/// COBS-encode a block (no delimiters added) — the inverse of [`cobs_decode`],
/// for senders framing payloads toward the board.
pub fn cobs_encode(data: &[u8]) -> Vec<u8> {
    let mut out = vec![0u8];
    let mut code_index = 0usize;
    let mut code = 1u8;
    for &byte in data {
        if byte == 0 {
            out[code_index] = code;
            code_index = out.len();
            out.push(0);
            code = 1;
        } else {
            out.push(byte);
            code += 1;
            if code == 0xFF {
                out[code_index] = code;
                code_index = out.len();
                out.push(0);
                code = 1;
            }
        }
    }
    out[code_index] = code;
    out
}

/// IEEE 802.3 CRC-32 (the Ethernet/zlib CRC), matching lib_crc32 on the board.
pub fn crc32(bytes: &[u8]) -> u32 {
    let mut crc = 0xFFFF_FFFFu32;
    for &b in bytes {
        crc ^= b as u32;
        for _ in 0..8 {
            crc = if crc & 1 != 0 {
                (crc >> 1) ^ 0xEDB8_8320
            } else {
                crc >> 1
            };
        }
    }
    crc ^ 0xFFFF_FFFF
}

/// Validate one delimiter-stripped segment: COBS-decode, check the CRC-32
/// trailer, return the envelope payload — `None` for anything malformed.
pub fn deframe(segment: &[u8]) -> Option<Vec<u8>> {
    let mut plain = cobs_decode(segment)?;
    if plain.len() < 4 {
        return None;
    }
    let cut = plain.len() - 4;
    let rx = u32::from_le_bytes(plain[cut..].try_into().unwrap());
    if crc32(&plain[..cut]) != rx {
        return None;
    }
    plain.truncate(cut);
    Some(plain)
}

/// Split a raw capture at 0x00 delimiters and return the envelope payloads of
/// the valid frames — the whole-buffer convenience over [`deframe`], for
/// snapshot captures (the SIL's sim USB buffer).
pub fn parse_frames(wire: &[u8]) -> Vec<Vec<u8>> {
    let mut deframer = Deframer::default();
    let mut out = deframer.push(wire);
    // Terminate any trailing segment, matching the split-at-delimiter shape.
    out.extend(deframer.push(&[0]));
    out
}

/// Frame an envelope payload for transmission: leading delimiter,
/// COBS(payload ‖ CRC-32 LE), trailing delimiter.
// [impl->app~conn_002~1]
pub fn frame(payload: &[u8]) -> Vec<u8> {
    let mut plain = payload.to_vec();
    plain.extend_from_slice(&crc32(payload).to_le_bytes());
    let mut wire = vec![0u8];
    wire.extend_from_slice(&cobs_encode(&plain));
    wire.push(0);
    wire
}

/// Discard bound for a delimiter-less garbage run: comfortably above the
/// board's frame cap plus COBS expansion, so no valid frame can trip it.
const DEFRAMER_MAX_SEGMENT_BYTES: usize = 4096;

/// Incremental deframer for a live byte stream: feed reads of arbitrary size,
/// collect the payload of every frame they complete. A partial frame stays
/// buffered across pushes; an invalid segment is discarded, costing only
/// itself.
// [impl->app~conn_002~1]
#[derive(Default)]
pub struct Deframer {
    buf: Vec<u8>,
}

impl Deframer {
    pub fn new() -> Self {
        Self::default()
    }

    /// Feed received bytes; returns the payloads of the frames they complete,
    /// in arrival order.
    pub fn push(&mut self, bytes: &[u8]) -> Vec<Vec<u8>> {
        self.buf.extend_from_slice(bytes);
        let mut out = Vec::new();
        // Deframe segments in place; one front-drain per push, not per frame.
        let mut start = 0usize;
        while let Some(rel) = self.buf[start..].iter().position(|&b| b == 0) {
            let end = start + rel;
            if end > start {
                if let Some(payload) = deframe(&self.buf[start..end]) {
                    out.push(payload);
                }
            }
            start = end + 1;
        }
        self.buf.drain(..start);
        // A delimiter-less run past any legal frame length is line noise;
        // drop it so the buffer cannot grow unbounded. The next delimiter
        // then terminates a truncated segment, which the CRC discards.
        if self.buf.len() > DEFRAMER_MAX_SEGMENT_BYTES {
            self.buf.clear();
        }
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn crc_check_value() {
        assert_eq!(crc32(b"123456789"), 0xCBF4_3926);
    }

    // [test->app~conn_002~1]
    #[test]
    fn frame_matches_reference_vector() {
        // The fw~conn_proto_002 byte-exact reference vector.
        let wire = frame(b"123456789");
        let mut expected = vec![0x00, 0x0E];
        expected.extend_from_slice(b"123456789");
        expected.extend_from_slice(&[0x26, 0x39, 0xF4, 0xCB, 0x00]);
        assert_eq!(wire, expected);
    }

    #[test]
    fn frame_round_trips_through_parse() {
        let payload = [0x11u8, 0x00, 0x22, 0x00, 0x33];
        let mut wire = frame(&payload);
        wire.extend_from_slice(&frame(b"second"));
        let frames = parse_frames(&wire);
        assert_eq!(frames, vec![payload.to_vec(), b"second".to_vec()]);
    }

    // [test->app~conn_002~1]
    #[test]
    fn corrupt_frame_dropped_valid_kept() {
        let mut wire = frame(b"good");
        let mut bad = frame(b"corrupt-me");
        bad[3] ^= 0x40;
        wire.extend_from_slice(&bad);
        wire.extend_from_slice(&frame(b"also good"));
        let frames = parse_frames(&wire);
        assert_eq!(frames, vec![b"good".to_vec(), b"also good".to_vec()]);
    }

    // [test->app~conn_002~1]
    #[test]
    fn deframer_reassembles_across_arbitrary_read_boundaries() {
        let mut wire = frame(b"first payload");
        wire.extend_from_slice(&frame(&[0x11, 0x00, 0x22]));
        wire.extend_from_slice(&frame(b"third"));

        // Every chunk size from 1 byte up must deliver the same three
        // payloads, whole and in order.
        for chunk in 1..=wire.len() {
            let mut deframer = Deframer::new();
            let mut got = Vec::new();
            for piece in wire.chunks(chunk) {
                got.extend(deframer.push(piece));
            }
            assert_eq!(
                got,
                vec![
                    b"first payload".to_vec(),
                    vec![0x11, 0x00, 0x22],
                    b"third".to_vec()
                ],
                "chunk size {chunk}"
            );
        }
    }

    #[test]
    fn deframer_discards_delimiterless_noise() {
        let mut deframer = Deframer::new();
        let noise = vec![0x55u8; DEFRAMER_MAX_SEGMENT_BYTES + 1];
        assert!(deframer.push(&noise).is_empty());
        // The buffer was dropped: a following valid frame still arrives.
        let got = deframer.push(&frame(b"after noise"));
        assert_eq!(got, vec![b"after noise".to_vec()]);
    }
}
