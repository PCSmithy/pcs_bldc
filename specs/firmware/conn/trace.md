---
status: draft
tags: [firmware, conn, trace, server]
---

# Signal trace services

The trace services of the app_server module ([[server]]): a host-set
watch list of memory spans sampled at per-entry periods in the PWM
cycle callback and streamed as `Samples` messages, one-shot memory
reads and writes, and the trace capability report.

## Configuration

### Trace resource configuration
`fw~conn_trace_001~1`

The server shall take the trace services' resources from its
configuration — the readable memory regions, the writable memory
regions, the watch capacity in entries, the sample-RAM budget in bytes
per millisecond, and the link budget in bytes per second — returning
false at initialization when the configuration is rejected by any of:

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
| Entry period | Not 1, 20, or 200 PWM cycles |
| Entry count | Exceeds the watch capacity (`fw~conn_trace_001~1`) |
| Samples fit | Any $S_g$ exceeds the 256-byte `Samples` data capacity (`fw~conn_trace_005~1`) |
| RAM usage | $u$ exceeds the sample-RAM budget (`fw~conn_trace_001~1`) |
| Link rate | $r$ exceeds the link budget (`fw~conn_trace_001~1`) |

where, over the requested entries with sizes $s_i$ bytes grouped by
period into groups $g$ of period $c_g$ cycles, record size
$S_g = \sum_{i \in g} s_i$, record rate $f_g = 20000 / c_g$ per second,
and records per millisecond $n_g = \max(1,\ 20 / c_g)$:

$$u = \sum_g n_g \,(4 + S_g) \quad \text{[bytes per millisecond]}$$

$$r = \sum_g \Bigl( S_g f_g + W \cdot m_g \Bigr), \quad
m_g = \min\Bigl(f_g,\ \max\bigl(1000,\ f_g S_g / 256\bigr)\Bigr)
\quad \text{[bytes per second]}$$

with $W$ the per-message wire overhead of `fw~conn_trace_005~1`, $m_g$
the group's message rate per `fw~conn_trace_009~1`, and an empty list
having $u = 0$ and $r = 0$.

Acceptance:

- An accepted request replaces the active list in full: sampling
  (`fw~conn_trace_004~1`) follows only the new list.
- A rejected request leaves the active list unchanged: sampling
  continues per the prior list.
- Each rejection condition rejects the request.
- A list whose $u$ equals the sample-RAM budget, and one whose $r$
  equals the link budget, are each accepted.

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
server shall sample it in the bridge cycle callback
(`fw~io_bridge_007~1`), after that cycle's commutation step
(`fw~mc_015~1`), per:

| Behavior | Detail |
|----------|-------|
| Cycle index | Sampling advances a 32-bit PWM-cycle index each callback, restarted at zero when a list installs, buffered samples of the prior list discarded |
| Capture | Each cycle captures every group that is due — a group of period $c$ cycles and offset $k$ is due when $(\text{index} - k) \bmod c = 0$ — the group's entries captured as one coherent snapshot into one buffered record |
| Overflow | A record that does not fit in the free space of the sample buffer — its size the sample-RAM budget (`fw~conn_trace_001~1`) — is skipped whole, and skipping holds until the buffer has drained to half its size, so a loss is one contiguous gap |

| Group period $c$ | Offset $k$ |
|------------------|------------|
| 1 cycle | 0 |
| 20 cycles (1 ms) | 1 |
| 200 cycles (10 ms) | 2 |

Acceptance:

- With a one-cycle entry, consecutive emitted records carry consecutive
  cycle indices, each with the entry's bytes.
- Entries at 1, 20, and 200 cycles appear in exactly the cycles their
  group's offset and period select.
- Two locations the firmware updates together within a cycle arrive
  mutually consistent in every capture.
- After a list installs, the first emitted record of each group carries
  that group's offset as its cycle index, and no prior-list record
  follows it.
- With emission stalled long enough to fill the sample buffer, emitted
  cycle indices jump past the skipped records and every emitted record
  holds a complete capture.
- Once a record is skipped, no record is admitted until the buffer has
  drained to half; the records then admitted form one gap-free run.

Covers:
- sys~obs_005~1

Needs: impl, test

### Sample emission
`fw~conn_trace_009~1`

Each millisecond, the server shall emit each group's buffered records
(`fw~conn_trace_004~1`) in capture order as `Samples` messages
(`fw~conn_trace_005~1`), consecutive records of one group whose cycle
indices step by its period sharing a message up to its data capacity,
a message starting only where the transmit capacity holds a full one
(records otherwise staying buffered), each emission taking only the
records buffered and the capacity present at its start, and leaving one
reply frame of transmit capacity (`fw~conn_server_001~1`) unused.

Acceptance:

- A one-cycle group with 16-byte records reaches the host as messages
  of up to 16 records, with no record held longer than 2 ms.
- A 10 ms group's records each arrive in their own message.
- A one-cycle group's records buffered across a 3 ms emission stall
  arrive in the next emission, in capture order.
- With transmit capacity for less than a full message, no message
  leaves; the buffered records arrive whole once capacity returns.

Covers:
- sys~obs_005~1

Needs: impl, test

### Samples message format
`fw~conn_trace_005~1`

A `Samples` message shall carry the records of one group
(`fw~conn_trace_009~1`) — the group's period in cycles, the cycle index
of its first record, its record count, and at most 256 data bytes, the
records concatenated in capture order, each record the group's spans in
watch-list order — its wire overhead beyond the data bytes at most
$W = 27$ bytes, the worst case over its encoding (`fw~conn_proto_001~1`)
and framing (`fw~conn_proto_002~1`):

| Component | Worst-case bytes |
|-----------|------------------|
| `request_id` (0 on stream messages, omitted on the wire) | 0 |
| `samples` field tag + length (`Envelope` field 33) | 4 |
| `period_cycles` field | 3 |
| `first_cycle` field | 6 |
| `count` field | 3 |
| `data` field tag + length | 3 |
| Frame CRC-32 | 4 |
| COBS overhead, $\lceil 279 / 254 \rceil$ | 2 |
| Frame delimiters | 2 |
| Total $W$ | 27 |

Acceptance:

- A known list and record batch encode to a byte-exact reference frame.
- The wire frame of a message carrying 256 data bytes is 283 bytes or
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
