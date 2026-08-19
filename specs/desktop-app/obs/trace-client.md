---
status: draft
tags: [app, obs]
---

# Trace client

The app side of the signal-trace stream: installing watch lists and
demultiplexing the returned `Samples` messages, against the firmware
services of [[../../firmware/conn/trace|trace]].

## Watch installation

### Watch installation
`app~obs_003~1`

The app shall install the selected signals (`app~obs_001~1`) as one
`WatchRequest` of their resolved address, size, and per-signal period
entries — each period 1 ms, 10 ms, or 100 ms (`sys~obs_005~1`) —
presenting the reply: the `TraceStatus` budgets and usage on
acceptance, the rejection `cause` otherwise.

Acceptance:

- The selected signals arrive at the device as one watch list, and an
  accepted install presents the reported budgets and usage.
- A rejected install presents the cause, and the previously installed
  watch list's stream continues to render (`fw~conn_trace_002~1`).

Covers:
- sys~obs_005~1
- sys~obs_009~1

Needs: impl, test

## Samples demultiplexing

### Samples demultiplexing
`app~obs_004~1`

The app shall demultiplex each received `Samples` message into
per-signal values by assigning its data bytes, in installed
watch-list order, to the signals whose period divides the message's
`tick_ms`, decoding each signal's bytes as its resolved scalar type.

Acceptance:

- With watches at 1 ms, 10 ms, and 100 ms periods, every received
  message's bytes map to exactly the due signals, in list order.
- A tick-count discontinuity yields values at exactly the received
  ticks.
- A signal's bytes decode per its scalar type: width, signedness, and
  floating-point format.

Covers:
- sys~obs_005~1

Needs: impl, test
