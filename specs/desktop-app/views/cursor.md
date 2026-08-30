---
status: draft
tags: [app, views]
---

# Cursor

The plot widgets' pointing model: one cursor time for every plot
widget, the pointed-trace emphasis while paused, and the paused
comparison anchor with its preview and deltas.

## Cursor time

### Shared cursor
`app~views_005~1`

The app shall hold at most one cursor time — set by pointing at any
plot widget, cleared when the pointer leaves every plot widget — each
plot widget marking a held time and listing, in its cursor readout,
its own signals' values at that time, rendered per the value
rendering table (`app~views_013~1`), a signal with no sample there
read out as absent.

Acceptance:

- Pointing at one plot widget places the cursor mark at the same time
  on every plot widget; leaving them clears every mark.
- Each plot's readout lists its signals' values at the held time; for
  a time inside a tick-count gap the readout states the absence.

See also: [[live-plot]], [[table]], [[value-rendering]]

Covers:
- sys~arch_002~1

Needs: impl, test

## Pointed trace

### Pointed trace emphasis
`app~views_012~1`

While the timeline (`app~views_008~1`) is paused, the app shall
emphasize the pointed trace — the pointed-at plot widget's trace whose
rendered line lies nearest the pointer, within 40 px — rendering it at
twice its stroke width and marking its row in that widget's cursor
readout (`app~views_005~1`), the emphasis transient and the signal's
appearance (`app~views_011~1`) unchanged.

Acceptance:

- Moving the pointer near one of several traces thickens that trace
  and marks its readout row; moving nearer a different trace
  transfers both.
- With no rendered line within 40 px of the pointer, no trace is
  emphasized.

Covers:
- sys~arch_002~1

Needs: impl, test

## Comparison

### Comparison anchor
`app~views_017~1`

A plot widget shall hold at most one comparison anchor — a sample of
one of its signals, marked on that widget alone by a vertical line
at the anchor's time and a horizontal line at the anchor's value,
placed against the anchor signal's axis (`app~views_007~1`) — set
and released per the actions below, whose anchor modifier is the
platform's:

| Platform | Anchor modifier |
|----------|-----------------|
| Windows | Ctrl |
| macOS | Command |

| Action | Behavior |
|--------|----------|
| Anchor modifier + primary-button click, paused (`app~views_008~1`), with a pointed trace (`app~views_012~1`) | The pointed signal's sample nearest in time to the click, the earlier of two equidistant, becomes the anchor, replacing any prior one |
| Primary-button press and release moving under 6 px, within 8 px of either anchor line | The anchor releases |
| Resume (`app~views_009~1`) | The anchor releases |
| The anchor's signal leaves the widget (`app~views_004~1`) | The anchor releases |

Acceptance:

- Each action row produces its behavior.
- The lines mark only the anchoring widget; a second plot widget
  anchors independently.

Covers:
- sys~arch_002~1

Needs: impl, test

### Comparison deltas
`app~views_018~1`

While a plot widget holds a comparison anchor (`app~views_017~1`)
and a cursor time is held (`app~views_005~1`), the widget's cursor
readout shall report the deltas per:

| Delta | Shown |
|-------|-------|
| Time — the cursor time minus the anchor's time, in ms | In the readout |
| Value — the anchoring widget's pointed trace's (`app~views_012~1`) sample at the cursor time minus the anchor's value, rendered per `app~views_013~1` | On the pointed row, while the pointed signal is assigned the anchor signal's axis (`app~views_007~1`), both signals of integer or float scalar type (`app~obs_001~1`), and the pointed signal holds a sample at the cursor time |

Acceptance:

- With an anchor held, a cursor time set from any plot widget shows
  the anchoring widget's readout carrying the time delta.
- The anchoring widget's pointed trace on the anchor's axis shows
  the pointed row's value delta; a pointed trace on the other axis,
  a boolean or enumeration signal, and a cursor time where the
  pointed signal has no sample each show none.

Covers:
- sys~arch_002~1

Needs: impl, test

### Anchor preview
`app~views_019~1`

While the timeline is paused (`app~views_008~1`) and the anchor
modifier (`app~views_017~1`) is held with a pointed trace
(`app~views_012~1`), a plot widget shall show a candidate mark — a horizontal line at the value of the pointed
signal's sample nearest the pointer in time, the earlier of two
equidistant — the mark following the pointer and the pointed trace,
and shown only while that condition holds.

Acceptance:

- With the anchor modifier held over a paused plot, the candidate
  mark sits at the nearest sample's value, moves with the pointer,
  and transfers with the pointed-trace emphasis.
- Releasing the anchor modifier removes the candidate mark and
  leaves the anchor state (`app~views_017~1`) as it was.

Covers:
- sys~arch_002~1

Needs: impl, test
