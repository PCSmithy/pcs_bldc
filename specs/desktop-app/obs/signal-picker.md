---
status: draft
tags: [app, obs]
---

# Signal selection from firmware debug data

### Signal selection from firmware debug data
`app~obs_001~1`

The app shall enumerate the static variables of the loaded firmware
ELF from its DWARF debug data — file-local definitions from every
linked module, and aggregate members by path (`a.b[2].c`) —
presenting them for selection by source name and resolving each
selected signal to its link-time address, byte size, and scalar type.

Acceptance:

- Statics from every linked firmware module, including file-local
  definitions and aggregate members by path, appear under their
  source names.
- A selection's resolved address, size, and type equal the ELF's
  debug entries for that variable.

See also: [[access-gate]], [[trace-client]]

Covers:
- sys~obs_002~1

Needs: impl, test
