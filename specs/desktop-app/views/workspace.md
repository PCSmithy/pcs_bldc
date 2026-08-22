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
| Restart the app | The arrangement, each widget's signals, and each signal's trace appearance ([[live-plot]]) are restored |

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
