---
status: draft
tags: [system, obs]
---

# Text log stream

### Text log stream
`sys~obs_007~1`

Text the firmware emits through standard C output (`printf`) shall
reach the desktop app in protocol log messages (`sys~conn_001~1`) and
be displayed by the app as a text log in emission order.

Acceptance:

- Strings printed in sequence by the firmware appear in the app's text
  log complete and in the same sequence.
- Captured protocol traffic carries the printed text in the schema's
  log message type.

See also: [[status-telemetry]]

Covers:

- (project goal: README.md, "Modeling, simulation, and observability
  infrastructure" — live inspection via the desktop visualizer /
  control app.)

Needs: fw, app, test
