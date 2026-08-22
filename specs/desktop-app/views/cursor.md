---
status: draft
tags: [app, views]
---

# Cursor

The plot widgets' pointing model: one cursor time for every plot
widget, and the pointed-trace emphasis while paused.

## Cursor time

### Shared cursor
`app~views_005~1`

The app shall hold at most one cursor time — set by pointing at any
plot widget, cleared when the pointer leaves every plot widget — each
plot widget marking a held time and listing, in its cursor readout,
its own signals' values at that time, a signal with no sample there
read out as absent.

Acceptance:

- Pointing at one plot widget places the cursor mark at the same time
  on every plot widget; leaving them clears every mark.
- Each plot's readout lists its signals' values at the held time; for
  a time inside a tick-count gap the readout states the absence.

See also: [[live-plot]], [[table]]

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
