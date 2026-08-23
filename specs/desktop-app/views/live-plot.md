---
status: draft
tags: [app, views]
---

# Plot widget

The workspace's strip-chart widget ([[workspace]]).

## Rendering

### Live plot
`app~views_001~1`

A plot widget shall plot each of its signals' demultiplexed
values (`app~obs_004~1`) against their tick timestamps as one trace
per signal in the signal's trace appearance (`app~views_011~1`),
extending as messages arrive, each gap-free run of ticks rendered as
one connected segment, its points rendered per `app~views_014~1`.

Acceptance:

- A trace spanning a tick-count gap renders as separate segments with
  no line across the gap.

Covers:
- sys~arch_002~1

Needs: impl, test

### Trace decimation
`app~views_014~1`

A plot widget shall render each trace's points by pixel column of the
plotted X range:

| Samples in a pixel column | Rendered points |
|---------------------------|-----------------|
| More than two | The column's minimum and maximum samples |
| At most two | The column's samples |

Acceptance:

- A column holding more than two samples renders exactly its minimum
  and maximum samples, a single-sample spike preserved.
- Zooming while paused (`app~views_009~1`) until every column holds
  at most two samples renders every sample.

Covers:
- sys~arch_002~1

Needs: impl, test

## Axes

### Axis assignment and scaling
`app~views_007~1`

A plot widget shall render each signal against its assigned axis —
left or right, set per signal in the widget's axes configuration — the
widget rendering one Y axis per assigned side in use, each axis
carrying its own labels and ranging per its scale mode, also set in
the axes configuration:

| Scale mode | Axis range |
|------------|------------|
| Auto | The extent of the axis's signals' rendered values, following as they change |
| Manual | The minimum to maximum set in the axes configuration |

Acceptance:

- With all signals assigned to the same axis, a single labeled Y axis
  renders; with signals assigned to both, two axes render with
  distinct labels, each signal against its own.
- Values moving beyond an auto axis's range extend the axis; the same
  movement beyond a manual axis leaves its bounds unchanged.

Covers:
- sys~arch_002~1

Needs: impl, test

## Trace appearance

### Trace appearance
`app~views_011~1`

The app shall render a signal's trace, on every plot widget holding
it, per the signal's appearance, set in a widget's configuration menu:
its color — an automatically assigned trace color, assigned when the
signal is first selected and held stable until changed — a solid,
dotted, or dashed line; an optional dot at each rendered sample
(`app~views_014~1`); and an interpolation, applied between
consecutive rendered samples within each gap-free run of ticks
(`app~views_001~1`):

| Interpolation | Rendering |
|---------------|-----------|
| Zero-order hold | Each value holds as a step until the next sample |
| Linear | Straight segments between consecutive samples |
| Monotone cubic | A cubic curve through the samples that never overshoots them |

Acceptance:

- A changed line style, dots setting, and interpolation each render
  per their setting on every plot widget holding the signal.
- A changed color renders wherever the signal's trace color renders,
  the watch panel's row included (`app~views_010~1`).

Covers:
- sys~arch_002~1

Needs: impl, test
