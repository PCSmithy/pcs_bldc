---
status: draft
tags: [app, views]
---

# Live plot

### Live plot
`app~views_001~1`

The app shall plot each traced signal's demultiplexed values
(`app~obs_004~1`) against their tick timestamps, appending points as
messages arrive and leaving values across a tick-count gap
unconnected.

Acceptance:

- Each traced signal renders as its own series whose points are the
  received (tick, value) pairs, extending while the stream runs.
- Values across a tick-count gap render unconnected.

Covers:
- sys~arch_002~1

Needs: impl, test
