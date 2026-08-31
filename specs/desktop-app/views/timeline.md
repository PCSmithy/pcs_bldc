---
status: draft
tags: [app, views]
---

# Timeline

The plot widgets' shared time base ([[live-plot]]): one span and one
live/paused mode for every plot widget, and the paused-range controls.

### Timeline
`app~views_008~1`

The app shall hold one plot timeline — a display span selected from
5 s, 10 s, 30 s, and 60 s, and a mode, live or paused — every plot
widget rendering the same X range per:

| Mode | X range |
|------|---------|
| Live | The most recent span of the stream, following as samples arrive |
| Paused | Initially the whole span preceding the pause; thereafter the range set by the paused-range controls (`app~views_009~1`) |

While paused, the paused span's samples are retained for the whole
pause, and newly arriving samples (`app~obs_004~1`) are retained for
the first 120 s of the pause — a longer pause surfacing the unretained
interval as a tick-count gap on resume.

Acceptance:

- Selecting a span renders that duration across every plot widget's
  X axis.
- Pausing freezes every plot widget at the same range, and the whole
  paused span renders however long the pause holds.
- Resuming within 120 s renders the live span including the samples
  that arrived while paused; resuming later renders the unretained
  interval as a tick-count gap.

See also: [[workspace]]

Covers:
- sys~arch_002~1

Needs: impl, test

### Paused range control
`app~views_009~1`

While the timeline (`app~views_008~1`) is paused, the app shall adjust
every plot widget's shared X range per the actions below, the range
bounded between 10 ms and the paused span:

| Action | Behavior |
|--------|----------|
| Vertical scroll over a plot widget, cursor time held | The range scales by a factor of 1.0015 per unit of wheel delta about the cursor time (`app~views_005~1`) |
| Vertical scroll over a plot widget, no cursor time held | The range scales by a factor of 1.0015 per unit of wheel delta about its center |
| Horizontal scroll over a plot widget | The range pans at its scale |
| Primary-button horizontal drag across a plot widget's canvas | Every plot widget's range becomes the dragged X extent |
| Resume | The timeline returns to live (`app~views_008~1`) |

Acceptance:

- A wheel step over one plot widget rescales the range on every plot
  widget, about the held cursor time or, with none held, the range
  center.
- Panning stops at the paused span's edges, zooming out at the span,
  and zooming in at 10 ms; a step into any bound leaves the range
  unchanged.
- A horizontal drag on one plot widget sets every plot widget's range
  to the dragged extent.

Covers:
- sys~arch_002~1

Needs: impl, test
