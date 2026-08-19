---
status: draft
tags: [firmware, conn, trace, server]
---

# Signal trace services

The trace services of the app_server module ([[server]]): a host-set
watch list of memory spans sampled at per-entry periods and streamed
as `Samples` messages, one-shot memory reads and writes, and the trace
capability report.

See also: the system specs behind these services — signal trace
(`sys~obs_005~1`), signal write (`sys~obs_006~1`), memory read
(`sys~obs_008~1`), trace capability report (`sys~obs_009~1`).

## Configuration

### Trace resource configuration
`fw~conn_trace_001~1`

The server shall take the trace services' resources from its
configuration — the readable memory regions, the writable memory
regions, the watch capacity in entries, the sample-RAM budget in
bytes, and the link budget in bytes per second — returning false at
initialization when the configuration is rejected by any of:

| Element | Rejected when |
|---------|---------------|
| Readable regions | The set is empty, or a region has zero length |
| Writable regions | A region has zero length |
| Watch capacity | Zero |
| Budgets | The sample-RAM budget or the link budget is zero |

Acceptance:

- A configuration declaring a readable region and nonzero budgets
  initializes, returning true.
- Each rejection condition returns false.

Covers:
- sys~obs_005~1

Needs: impl, test

## Watch list

### Watch-list admission
`fw~conn_trace_002~1`

A `WatchRequest` shall replace the active watch list in full when
accepted and leave it unchanged when rejected, rejecting the request
when:

| Element | Rejected when |
|---------|---------------|
| Entry span | Not contained in one readable region (`fw~conn_trace_001~1`) |
| Entry size | Outside 1..8 bytes |
| Entry period | Not 1 ms, 10 ms, or 100 ms |
| Entry count | Exceeds the watch capacity (`fw~conn_trace_001~1`) |
| Samples fit | $\sum_i s_i$ exceeds the 256-byte `Samples` data capacity (`fw~conn_trace_005~1`) |
| RAM usage | $u$ exceeds the sample-RAM budget (`fw~conn_trace_001~1`) |
| Link rate | $r$ exceeds the link budget (`fw~conn_trace_001~1`) |

where, over the requested entries with sizes $s_i$ bytes, periods
$p_i$ ms, and per-entry sample rates $f_i = 1000 / p_i$ per second:

$$u = 4 + \sum_i s_i \quad \text{[bytes]}$$

$$r = \Bigl(\sum_i s_i f_i\Bigr) + \Bigl(W \cdot \max_i f_i\Bigr)
\quad \text{[bytes per second]}$$

with $W$ the per-message wire overhead of `fw~conn_trace_005~1`, and
an empty list having $u = 0$ and $r = 0$.

Acceptance:

- An accepted request replaces the active list in full: sampling
  (`fw~conn_trace_004~1`) follows only the new list.
- A rejected request leaves the active list unchanged: sampling
  continues per the prior list.
- Each rejection condition rejects the request.
- A list whose $r$ equals the link budget is accepted.

Covers:
- sys~obs_005~1

Needs: impl, test

### Watch-list clear on disconnect
`fw~conn_trace_003~1`

The server shall clear the active watch list when its serial channel
loses the host connection (`fw~conn_serial_005~1`).

Acceptance:

- With a watch list active, a disconnect followed by a reconnect
  produces no `Samples` message until a new `WatchRequest` is accepted.
- After the reconnect, the reported usage (`fw~conn_trace_006~1`) is
  zero.

Covers:
- sys~obs_005~1

Needs: impl, test

## Sampling and streaming

### Watch sampling
`fw~conn_trace_004~1`

While the active watch list (`fw~conn_trace_002~1`) is non-empty, the
server shall sample and stream it per:

| Behavior | Detail |
|----------|-------|
| Tick | Sampling advances a millisecond tick count, restarted at zero when a list installs, buffered samples of the prior list discarded |
| Capture | Each tick captures every entry whose period divides the tick count, entries sharing a tick captured as one coherent snapshot |
| Emission | Each captured tick is emitted as one `Samples` message (`fw~conn_trace_005~1`), in capture order |
| Overflow | A tick whose capture does not fit in the free space of the sample buffer — its size the sample-RAM budget (`fw~conn_trace_001~1`) — is skipped whole |

Acceptance:

- With a 1 ms entry, consecutive emitted messages carry consecutive
  tick counts, each with the entry's bytes.
- Entries at 1 ms, 10 ms, and 100 ms appear in exactly the ticks their
  periods divide.
- Two locations the firmware updates together within a tick arrive
  mutually consistent in every capture.
- After a list installs, the first emitted message carries tick zero,
  and no prior-list message follows it.
- With emission stalled long enough to fill the sample buffer, emitted
  tick counts jump past the skipped ticks and every emitted message
  holds complete captures.

Covers:
- sys~obs_005~1

Needs: impl, test

### Samples message format
`fw~conn_trace_005~1`

A `Samples` message shall carry one captured tick — the 32-bit tick
count and at most 256 data bytes, the captured spans concatenated in
watch-list order — its wire overhead beyond the data bytes at most
$W = 21$ bytes, the worst case over its encoding (`fw~conn_proto_001~1`)
and framing (`fw~conn_proto_002~1`):

| Component | Worst-case bytes |
|-----------|------------------|
| `request_id` (0 on stream messages, omitted on the wire) | 0 |
| `samples` field tag + length (`Envelope` field 33) | 4 |
| `tick_ms` field | 6 |
| `data` field tag + length | 3 |
| Frame CRC-32 | 4 |
| COBS overhead, $\lceil 273 / 254 \rceil$ | 2 |
| Frame delimiters | 2 |
| Total $W$ | 21 |

Acceptance:

- A known list and tick encode to a byte-exact reference frame.
- The wire frame of a message carrying 256 data bytes is 277 bytes or
  fewer.

Covers:
- sys~obs_005~1

Needs: impl, test

## Status

### Trace capability report
`fw~conn_trace_006~1`

The server shall answer an accepted `WatchRequest` and any
`TraceStatusRequest` with a `TraceStatus` reply (`fw~conn_server_001~1`)
reporting the configured sample-RAM and link budgets
(`fw~conn_trace_001~1`) and the active list's usage $u$ and $r$
(`fw~conn_trace_002~1`).

Acceptance:

- With a known list active, a `TraceStatusRequest` returns the
  configured budgets and the list's computed $u$ and $r$.
- The reply to an accepted `WatchRequest` reports the newly installed
  list's usage.

Covers:
- sys~obs_009~1

Needs: impl, test

## One-shot access

### Memory read
`fw~conn_trace_007~1`

An accepted `ReadRequest` shall be answered (`fw~conn_server_001~1`)
with a `ReadReply` carrying the current contents of the requested span,
the request rejected when:

| Rejected when |
|---------------|
| The size is outside 1..128 bytes |
| The span is not contained in one readable region (`fw~conn_trace_001~1`) |

Acceptance:

- A read of the span at a firmware variable's address and size returns
  that variable's current value.
- Each rejection condition rejects the request.

Covers:
- sys~obs_008~1

Needs: impl, test

### Memory write
`fw~conn_trace_008~1`

An accepted `WriteRequest` shall write its data to the requested span
once, firmware readers observing the span's prior contents or the
written value in full, the request rejected when:

| Rejected when |
|---------------|
| The data length is outside 1..8 bytes |
| The span is not contained in one writable region (`fw~conn_trace_001~1`) |

Acceptance:

- A written variable read back (`fw~conn_trace_007~1`) returns the
  written value.
- In the scenario of `sys~obs_006~1` writing a multi-byte variable the
  firmware reads every millisecond, every read observes the prior or
  the written value in full.
- Each rejection condition rejects the request.

Covers:
- sys~obs_006~1

Needs: impl, test
