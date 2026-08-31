---
status: draft
tags: [system, obs]
---

# Trace capability report

### Trace capability report
`sys~obs_009~1`

The firmware shall report, on request over the protocol
(`sys~conn_001~1`), its signal-trace (`sys~obs_005~1`) resource budgets
and the active watch list's usage of each.

Acceptance:

- The reported budgets match the firmware's configured values.
- With no watches active the reported usage is zero; with a non-empty
  watch list installed it is nonzero.

See also: [[signal-trace]]

Covers:

- (project goal: README.md, "Modeling, simulation, and observability
  infrastructure" — rich USB-streamed telemetry at meaningful rates,
  paired with a desktop visualizer / control app.)

Needs: fw, app, test
