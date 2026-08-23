---
status: draft
tags: [app, views]
---

# Workspace

### Workspace
`app~views_004~1`

The app shall compose its workspace from plot ([[live-plot]]) and
table ([[table]]) widgets per:

| Action | Behavior |
|--------|----------|
| Drop a signal on empty canvas | A plot widget holding the signal appears at the drop position |
| Drop a signal on an existing widget | The signal joins that widget |
| Add-table action | An empty table widget appears |
| Drag a widget's header | The widget moves to any canvas position, snapped to the 50 px layout grid |
| Drag a widget's corner handle | The widget resizes, both extents snapped to the 50 px layout grid |
| Remove a signal via the widget's configuration menu | The signal leaves the widget's signal set |
| Restart the app | The arrangement, each widget's signals, each widget's title (`app~views_016~1`), and each signal's trace appearance ([[live-plot]]) are restored |

Acceptance:

- Each action row produces its behavior.
- Removing a widget's last signal leaves the widget in place showing
  its drop hint.
- After moving and resizing widgets and restarting the app, the
  arrangement, widget contents, and per-signal trace appearance match
  the pre-restart state.

Covers:
- sys~arch_002~1

Needs: impl, test

### Widget title
`app~views_016~1`

A widget shall carry an editable title, shown in its header and set
by committing a name there, a committed empty name unsetting it; an
unset title reads per:

| Widget state | Unset title |
|--------------|-------------|
| Plot holding signals | Its earliest-added signal's leaf name (the path's final segment) |
| Plot holding no signals | `Plot` |
| Table | `Live values` |

Acceptance:

- Each widget-state row shows its unset title; removing the
  earliest-added signal moves the title to the next remaining signal.
- A committed name replaces the unset title until a committed empty
  name returns it.

Covers:
- sys~arch_002~1

Needs: impl, test
