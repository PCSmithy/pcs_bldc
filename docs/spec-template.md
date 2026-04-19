---
status: example
tags: [template]
---

# Spec template

This file is a worked example of the spec format. **It is not a real spec**
and should be skipped by `oft trace` invocations against `specs/` (it
contains no real `Covers:` parents). Copy its structure when authoring new
specs.

All spec IDs follow the project convention defined in
[`spec-system.md`](spec-system.md#id-conventions):
`<type>~<topic>_<NNN>~1`. Topic abbreviations are in the canonical table
in that doc. Numbering is sequential per `(type, topic)` tuple. The
trailing `~1` is fixed by project policy.

## A `fw~` (firmware) spec

```markdown
---
status: draft
tags: [firmware, foc]
---

# Park transform

### Park transform
`fw~mc_001~1`

The FOC inner loop shall transform the measured three-phase stator currents
into the rotor-aligned (d, q) frame using the Park transform, with the rotor
electrical angle provided by the position estimator.

Acceptance:
- Computed (i_d, i_q) match a reference Python implementation to within
  numerical noise on the standard SIL trajectory.
- Worst-case execution time within the inner-loop timing budget defined in
  `fw~mc_002~1`.

See also: [[clarke-transform]], [[foc-architecture]]

Covers:
- sys~mc_001~1

Needs: impl, test
```

## An `app~` (desktop GUI) spec

```markdown
---
status: draft
tags: [desktop-app, connection]
---

# Device discovery

### Device discovery
`app~conn_001~1`

The desktop application shall enumerate all connected pcs_bldc devices on
startup and present them to the user as a selectable list.

Acceptance:
- With one device connected, the list contains exactly that device with
  its serial number and firmware version visible.
- With no devices connected, the list is empty and a clear "no device
  found" message is shown.
- Hot-plugging a device updates the list within 2 seconds.

Covers:
- sys~conn_001~1

Needs: impl, test
```

## A cross-component `sys~` spec

```markdown
---
status: draft
tags: [system, configuration, mvp]
---

# Motor parameter configuration

### Motor parameter configuration
`sys~persist_002~1`

The user shall be able to configure motor parameters (R, L, Kt, J, B,
encoder offset) via the desktop application, and those parameters shall
persist across firmware power cycles.

Acceptance:
- Setting a parameter via the desktop app, power-cycling the device, and
  reading the parameter back returns the set value.
- Power-cycling without a prior set returns the last persisted value (or
  factory default on first boot).

Covers:
- (project goal: README.md, "State estimation" — initial parameter
  measurement / hard-coded values to get spinning)

Needs: fw, app, test
```

## Downstream code and test tags

```c
// [impl->fw~mc_001~1]
void foc_park_transform(...) { ... }
```

```rust
// [impl->app~conn_001~1]
fn discover_devices() -> Vec<Device> { ... }
```

```python
# [test->fw~mc_001~1]
def test_park_transform_matches_reference(): ...

# [test->sys~persist_002~1]
def test_settings_round_trip_across_power_cycle(): ...
```

## What stays out

- No five-section IEEE template, no rationale-and-justification blocks.
- No `Author:` / `Date:` metadata — git tracks that.
- No "this spec covers itself" recursive `Covers:` — OFT will reject it.
- No multiple "shall" statements per spec — split into multiple specs.

## OFT gotchas

- **Always include a language hint on opening code fences.** Use `` ```text ``
  for ASCII / box-drawing diagrams; use `` ```c `` / `` ```rust `` /
  `` ```python `` for example code. A bare opening `` ``` `` (no language)
  confuses OFT's markdown parser and can prevent it from detecting any specs
  in the rest of the file. The closing fence stays plain `` ``` ``.
- **Avoid `---` thematic breaks in spec body text.** They can be misread as
  another frontmatter delimiter. Use a heading (`## Section`) for visual
  separation instead.
- **Keep the spec ID line directly under its heading**, with no blank line
  between them: `### Heading` then on the next line `` `type~name~version` ``.
