---
status: draft
tags: [app, views]
---

# Watch panel

### Watch panel
`app~views_010~1`

The app shall list each signal on the watch list in the signal
picker's watch panel — a row of the signal's trace color, name,
sample period, and latest streamed value, the value rendered per the
value rendering table (`app~views_013~1`) — the period selectable and
the signal removable in the row, either edit applied through the
watch installation (`app~obs_003~1`).

Acceptance:

- A row appears when its signal joins the watch list and leaves when
  the signal is unwatched, its value cell tracking the newest sample.
- Editing a row's period, and removing a row, each reach the device
  as one recommitted watch list (`app~obs_003~1`).

See also: [[../obs/signal-picker|signal-picker]], [[table]], [[value-rendering]]

Covers:
- sys~arch_002~1

Needs: impl, test
