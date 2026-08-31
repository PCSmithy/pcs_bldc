---
status: draft
tags: [system, conn]
---

# Schema-defined protocol

### Schema-defined protocol
`sys~conn_001~1`

All data exchanged between the firmware and the desktop app shall be
messages defined in a shared protocol-buffer schema, carried over the
transport defined in `sys~arch_003~1` and organized as services composed
of request/response operations and device-to-host streams.

Acceptance:

- Firmware-emitted messages decode in a host decoder generated from the
  schema, and app-emitted requests decode in the firmware.
- A stream defined in the schema delivers a sequence of schema-defined
  messages from the firmware to the app.

See also: [[framing]], [[request-acknowledgement]]

Covers:

- (project goal: README.md, "Modeling, simulation, and observability
  infrastructure" — live inspection, command, and tuning via the desktop
  visualizer / control app.)

Needs: fw, app, test
