//! Wire-level helpers for the board's framed protocol (fw~conn_proto_002):
//! COBS-delimited frames carrying an Envelope followed by its CRC-32. Shared
//! by every SIL test that reads or feeds the sim USB capture. Full protobuf
//! decode of the envelope payloads arrives with the desktop-app work.

use voyant::Firmware;

/// Leading wire bytes of an Envelope whose payload is board.Telemetry
/// (shared.proto field 62, length-delimited: varint tag 0xF2 0x03). An
/// envelope with request_id 0 encodes starting at its oneof tag.
pub const TELEMETRY_TAG_BYTES: [u8; 2] = [0xF2, 0x03];

/// Read the raw sim USB TX capture buffer (txLen + tx[] bytes) by DWARF.
pub fn read_tx_capture(fw: &Firmware) -> Vec<u8> {
    let len = fw.read_cvar("HW_USB_sim_data.txLen").as_u64().unwrap_or(0);
    let mut bytes = Vec::with_capacity(len as usize);
    for i in 0..len {
        let b = fw
            .read_cvar(&format!("HW_USB_sim_data.tx[{i}]"))
            .as_u64()
            .unwrap_or(0) as u8;
        bytes.push(b);
    }
    bytes
}

/// Decode one COBS block (delimiters already stripped); `None` on malformed
/// input.
pub fn cobs_decode(seg: &[u8]) -> Option<Vec<u8>> {
    let mut out = Vec::new();
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
/// for tests that inject frames toward the board.
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
            crc = if crc & 1 != 0 { (crc >> 1) ^ 0xEDB8_8320 } else { crc >> 1 };
        }
    }
    crc ^ 0xFFFF_FFFF
}

/// Split a raw capture at 0x00 delimiters, COBS-decode each segment, verify
/// the CRC-32 trailer, and return the envelope payloads of the valid frames.
pub fn parse_frames(wire: &[u8]) -> Vec<Vec<u8>> {
    let mut frames = Vec::new();
    for seg in wire.split(|&b| b == 0).filter(|s| !s.is_empty()) {
        if let Some(plain) = cobs_decode(seg) {
            if plain.len() >= 4 {
                let (payload, trailer) = plain.split_at(plain.len() - 4);
                let rx = u32::from_le_bytes([trailer[0], trailer[1], trailer[2], trailer[3]]);
                if crc32(payload) == rx {
                    frames.push(payload.to_vec());
                }
            }
        }
    }
    frames
}

/// Frame an envelope payload for injection toward the board: leading
/// delimiter, COBS(payload ‖ CRC-32 LE), trailing delimiter.
pub fn frame(payload: &[u8]) -> Vec<u8> {
    let mut plain = payload.to_vec();
    plain.extend_from_slice(&crc32(payload).to_le_bytes());
    let mut wire = vec![0u8];
    wire.extend_from_slice(&cobs_encode(&plain));
    wire.push(0);
    wire
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn crc_check_value() {
        assert_eq!(crc32(b"123456789"), 0xCBF4_3926);
    }

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

    #[test]
    fn corrupt_frame_dropped_valid_kept(){
        let mut wire = frame(b"good");
        let mut bad = frame(b"corrupt-me");
        bad[3] ^= 0x40;
        wire.extend_from_slice(&bad);
        wire.extend_from_slice(&frame(b"also good"));
        let frames = parse_frames(&wire);
        assert_eq!(frames, vec![b"good".to_vec(), b"also good".to_vec()]);
    }
}
