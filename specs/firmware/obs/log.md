---
status: draft
tags: [firmware, obs, log]
---

# Log stream

The firmware side of the text log stream: standard-output text is
captured into a bounded buffer by the app_server module
([[../conn/server]]) and emitted over the protocol.

### Log capture
`fw~obs_log_001~1`

The firmware shall capture bytes written through standard C output
(`printf`) into a 512-byte buffer, discarding the oldest buffered byte
for each byte captured while the buffer is full.

Acceptance:
- Bytes printed within the buffer capacity are emitted
  (`fw~obs_log_002~1`) in write order.
- After printing more than the buffer capacity, the emitted stream
  carries the newest 512 bytes.

Covers:
- sys~obs_007~1

Needs: impl, test

### Log emission
`fw~obs_log_002~1`

While a host holds the device's serial port open
(`fw~conn_serial_005~1`), the firmware shall emit captured log bytes
(`fw~obs_log_001~1`) as `LogText` messages (`fw~conn_proto_001~1`) in
capture order, splitting runs longer than one message's capacity across
consecutive messages.

Acceptance:
- Two strings printed in sequence arrive in `LogText` payloads in the
  same sequence.
- Printed text longer than one `LogText` payload arrives intact across
  consecutive messages.
- Bytes printed while no host holds the port open are emitted, up to
  the buffer capacity, once a host opens it.

Covers:
- sys~obs_007~1

Needs: impl, test
