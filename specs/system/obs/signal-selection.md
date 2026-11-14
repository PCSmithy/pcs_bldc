---
status: draft
tags: [system, obs]
---

# Signal selection from firmware debug information

### Signal selection from firmware debug information
`sys~obs_002~1`

The desktop app shall present the scalar values of the static variables
enumerated from a firmware ELF's debug information for selection by name
as signals, resolving each selected signal to its address, size, and
scalar type.

Acceptance:

- Every static variable of a linked firmware module that holds a scalar
  value outside an array of more than 64 elements, file-local
  definitions included, appears in the selection list under its source
  name.
- A selected signal's resolved address, size, and type match the ELF's
  debug entries for that variable.

See also: [[signal-trace]], [[signal-write]], [[firmware-identity]]

Covers:

- (project goal: README.md, "Modeling, simulation, and observability
  infrastructure" — control loop internals, references, estimator
  states, raw sensor data.)

Needs: app, test
