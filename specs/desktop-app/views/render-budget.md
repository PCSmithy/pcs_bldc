---
status: draft
tags: [app, views]
---

# Render budget

### Render budget
`app~views_015~1`

Streaming the reference shape below for 60 s, the app shall render
within both budgets:

| Budget | Bound |
|--------|-------|
| Rendered frame rate | 60 rendered frames per second or above, averaged over every 1 s of the run |
| Rendered frame time | Every rendered frame completes within 33 ms |

| Reference-shape parameter | Value |
|---------------------------|-------|
| Watch list (`app~obs_003~1`) | 8 signals, each at a 10 ms sample period |
| Plot widgets ([[workspace]]) | 4, each holding two of the signals |
| Display span (`app~views_008~1`) | 30 s |
| App window | 1920 × 1080 CSS px at a device pixel ratio of 1 |

Acceptance:

- Both budget rows hold, measured from the rendered-frame cadence
  across the full 60 s run of the reference shape.

See also: [[live-plot]]

Covers:
- sys~arch_002~1

Needs: impl, test
