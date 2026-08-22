---
status: draft
tags: [app, obs]
---

# Signal picker

The picker's acquisition model: enumerating the firmware ELF's
namespace and narrowing it for selection.

## Selection

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

## Filtering

### Signal filter
`app~obs_005~1`

The app shall narrow the presented signals (`app~obs_001~1`) to those
matching a case-insensitive pattern, interpreted per:

| Pattern | Interpretation |
|---------|----------------|
| Literal characters only | Substring match |
| Literals and `*` only | `*` matches any run of characters |
| Any other | A regular expression; one that fails to compile matches as a substring |

Acceptance:

- A substring, a `*` pattern, and a regular expression each narrow
  the presented list to exactly their matches.
- An invalid regular expression matches as a substring.

Covers:
- sys~obs_002~1

Needs: impl, test

### Read-only exclusion
`app~obs_006~1`

The app shall provide a read-only exclusion, enabled and disabled in
the signal picker, that while enabled narrows the presented signals
to those resolved to writable storage — a link-time address in a
writable ELF section — composing with the pattern filter
(`app~obs_005~1`) as their intersection.

Acceptance:

- Enabling the exclusion presents exactly the writable signals among
  the current pattern's matches; disabling presents the read-only
  matches again.
- A watched signal hidden by the exclusion stays watched, its watch
  panel row (`app~views_010~1`) in place.

Covers:
- sys~obs_002~1

Needs: impl, test
