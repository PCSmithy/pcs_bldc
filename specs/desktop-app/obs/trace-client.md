---
status: draft
tags: [app, obs]
---

# Trace client

The app side of the signal-trace stream: installing watch lists and
demultiplexing the returned `Samples` messages, against the firmware
services of [[../../firmware/conn/trace|trace]].

### Watch installation
`app~obs_003~1`

The app shall install the selected signals (`app~obs_001~1`) as one
`WatchRequest` of their resolved address, size, and per-signal period
entries — each period one PWM cycle, 1 ms, or 10 ms (`sys~obs_005~1`) —
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

### Samples demultiplexing
`app~obs_004~1`

The app shall demultiplex each received `Samples` message into
per-signal values by splitting its data bytes into `count` records,
assigning each record's bytes, in installed watch-list order, to the
signals of the message's period at cycle index `first_cycle` plus the
record's ordinal times the period, decoding each signal's bytes as its
resolved scalar type. Each cycle index maps to a sample time of 50 µs
per cycle.

Acceptance:

- With watches at the one-cycle, 1 ms, and 10 ms periods, every
  received message's records map to exactly the signals of its period,
  in list order, at cycle indices spaced by that period.
- A cycle-index gap yields values at exactly the received indices.
- A signal's bytes decode per its scalar type: width, signedness, and
  floating-point format.

Covers:
- sys~obs_005~1

Needs: impl, test
