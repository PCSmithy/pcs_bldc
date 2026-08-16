---
status: draft
tags: [firmware, conn, proto, driver]
---

# Protocol framing and schema

The protocol stack over the serial byte stream: messages are defined in
protocol-buffer schemas — the reusable framework schema at
`sw/lib/c/shared/proto/`, importing the board schema at `sw/proto/` for
the envelope's fixed-name board payloads — and cross the wire as
COBS-delimited, CRC-validated frames carried by the `IO_COBSFrame`
io-layer frame driver over an `IO_serial` channel ([[serial]]).

See also: the system specs `sys~conn_001~1` (schema) and `sys~conn_002~1`
(framing).

## Schema

### Envelope schema
`fw~conn_proto_001~1`

The firmware shall exchange every protocol message as an `Envelope` — a
`request_id` field and a `oneof` of all protocol payload types —
reporting failure for received bytes that do not decode as an
`Envelope`.

Acceptance:
- An `Envelope` carrying each payload type encodes and decodes back to
  identical field values.
- Decoding a truncated `Envelope` encoding reports failure.

Covers:
- sys~conn_001~1

Needs: impl, test

## Frame codec

### Frame format
`fw~conn_proto_002~1`

Each frame on the wire shall consist of:

| Field | Content |
|-------|---------|
| Leading delimiter | `0x00` |
| Body | COBS encoding of: the encoded `Envelope` followed by the IEEE 802.3 CRC-32 of the encoded `Envelope`, little-endian |
| Trailing delimiter | `0x00` |

Acceptance:
- A known `Envelope` encodes to a byte-exact reference frame vector.
- The body contains no `0x00` byte.

Covers:
- sys~conn_002~1

Needs: impl, test

## Driver configuration and lifecycle

### Frame driver initialization and configuration validation
`fw~conn_proto_003~1`

The frame driver shall validate the supplied configuration before use
and initialize every configured channel, returning false if the
configuration is rejected by any of:

| Element | Rejected when |
|---------|---------------|
| Config | The config pointer or its channel array is NULL, or the channel count exceeds the available frame channels. |
| Channel | A channel names an unavailable backing serial channel, or declares a maximum frame length of zero. |

Acceptance:
- A valid config initializes every channel and returns true.
- Each rejection condition returns false.

Covers:
- sys~conn_002~1

Needs: impl, test

## Data transfer

### Whole-frame transmission
`fw~conn_proto_004~1`

On a channel, the frame driver shall hand a frame to the backing serial
channel only when the serial channel's free transmit capacity
(`fw~conn_serial_006~1`) holds the entire frame, transmitting accepted
frames in order and reporting the frame dropped otherwise.

Acceptance:
- A frame within free capacity is transmitted in full, in order with
  prior frames.
- A frame exceeding free capacity is dropped whole — no bytes of it
  reach the transport — and reported dropped.

Covers:
- sys~conn_002~1

Needs: impl, test

### Frame reception and resynchronization
`fw~conn_proto_005~1`

On a channel, the frame driver shall split bytes received from the
backing serial channel into `0x00`-separated segments and handle each
segment per:

| Segment | Handling |
|---------|----------|
| COBS-decodes with a passing CRC, within the channel's configured maximum frame length (`fw~conn_proto_003~1`) | Delivered to the consumer, in arrival order |
| Empty | Skipped |
| COBS error, CRC mismatch, or over the maximum frame length | Discarded |

Acceptance:
- Frames arriving back-to-back are delivered in order.
- A corrupted segment is discarded and the following valid frame is
  delivered.
- Bytes exceeding the maximum frame length with no delimiter are
  discarded and the next valid frame is delivered.

Covers:
- sys~conn_002~1

Needs: impl, test
