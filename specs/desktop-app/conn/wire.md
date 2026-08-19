---
status: draft
tags: [app, conn]
---

# Protocol client framing and envelope codec

The app side of the wire contract: COBS/CRC frames carrying protobuf
`Envelope`s, per the same format the firmware serves
([[../../firmware/conn/proto|proto]]).

## Frame codec

### Frame codec
`app~conn_002~1`

The app shall carry each protocol payload as a frame per:

| Direction | Behavior |
|-----------|----------|
| Transmit | Each payload encoded as a frame of the `fw~conn_proto_002~1` wire format |
| Receive | Received bytes split at `0x00` delimiters; each segment that COBS-decodes with a passing CRC-32 delivered in order; every other segment discarded |

Acceptance:

- A known payload encodes to the byte-exact reference frame of the
  `fw~conn_proto_002~1` format.
- A corrupted segment between two valid frames is discarded and both
  valid frames are delivered.
- Frames arriving split across arbitrary read boundaries are delivered
  whole and in order.

Covers:
- sys~conn_002~1

Needs: impl, test

## Envelope codec

### Envelope exchange
`app~conn_003~1`

The app shall exchange every protocol message as a schema `Envelope`
(`sys~conn_001~1`), assigning each transmitted request a `request_id`
unique among its outstanding requests and handling each received
envelope per:

| Received | Handling |
|----------|----------|
| A reply carrying an outstanding `request_id` | Resolved to that request — a `Response`'s verdict and `cause` (the `fw~conn_server_001~1` reply convention) delivered to the requester |
| A stream payload (log, telemetry, samples) | Dispatched to its consumer, independent of any request |

Acceptance:

- Concurrent outstanding requests each resolve to their own reply by
  `request_id`.
- A `Response` with `accepted` clear delivers its `cause` to the
  requester.
- Stream envelopes decode and dispatch with no outstanding request.

Covers:
- sys~conn_001~1

Needs: impl, test
