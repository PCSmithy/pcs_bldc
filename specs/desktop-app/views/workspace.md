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
| Add a plot via the workspace's widget launcher | A plot widget holding no signals appears at an unoccupied canvas position on the 50 px layout grid |
| Add a table via the workspace's widget launcher | A table widget holding no signals appears at an unoccupied canvas position on the 50 px layout grid |
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

### Widget selection and removal
`app~views_023~1`

The app shall select a widget on a click in it — one widget selected
at a time, marked by an accent border, a click on another widget moving
the selection and a click on empty canvas clearing it — and remove the
selected widget on the Delete key (Delete or Backspace on macOS) pressed
outside a text field; a widget removed by that key or by its menu's
Remove widget releases each of its signals held by no other widget from
the watch list (`app~obs_003~1`).

Acceptance:

- A click on a widget marks it selected; a click on another widget
  moves the mark, and a click on empty canvas clears it.
- Delete removes the selected widget, and the menu's Remove widget
  removes its widget; either way the widget's signals held by no other
  widget leave the watch list.
- Delete pressed while editing a widget title or typing in an input
  edits the text, and the widget stays.

Covers:
- sys~arch_002~1

Needs: impl, test
