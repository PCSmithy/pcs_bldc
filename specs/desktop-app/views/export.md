---
status: draft
tags: [app, views]
---

# Capture export

Writing a span of the plot history to a file for analysis outside the
app.

See also: [[timeline]] (the history and the selected range),
[[live-plot]] (the signals held).

### Capture export
`app~views_028~1`

The app shall write, on Export, the samples of every watched signal over
the timeline's selected range (`app~views_008~1`) — or over its visible
span with no range selected — to a comma-separated file the operator
names in a save dialog: one header row of `time_ms` followed by each
signal's path, one row per distinct sample time in the range, each
signal's cell its sample at that time or empty where it has none; Export
unavailable while no signal is watched.

Acceptance:
- With two signals watched at different periods over a 1 s selected
  range, the file holds a row for every sample time of either signal,
  the other signal's cell empty where it did not sample.
- The header names `time_ms` and both signals' paths.
- With no range selected, the rows span the timeline's visible span.

Covers:
- sys~arch_002~1

Needs: impl, test
