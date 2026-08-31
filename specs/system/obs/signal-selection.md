---
status: draft
tags: [system, obs]
---

# Signal selection from firmware debug information

### Signal selection from firmware debug information
`sys~obs_002~1`

The desktop app shall present the static variables enumerated from a
firmware ELF's debug information for selection by name as signals,
resolving each selected signal to its address, size, and scalar type.

Acceptance:

- Static variables from every linked firmware module, including
  file-local definitions, appear in the selection list under their
  source names.
- A selected signal's resolved address, size, and type match the ELF's
  debug entries for that variable.

See also: [[signal-trace]], [[signal-write]], [[firmware-identity]]

Covers:

- (project goal: README.md, "Modeling, simulation, and observability
  infrastructure" — control loop internals, references, estimator
  states, raw sensor data.)

Needs: app, test
