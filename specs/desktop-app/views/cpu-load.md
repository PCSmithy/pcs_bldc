---
status: draft
tags: [app, views]
---

# CPU load

A widget that shows how the firmware's processor time splits between its
tasks, its idle task, and its cycle interrupt, from cumulative
microsecond counters it watches like any other signal.

See also: [[workspace]] (widgets and their persisted configuration),
[[live-plot]] (the watch flow the counters share).

### CPU load donut
`app~views_020~1`

A CPU load widget shall show, as a donut with one wedge per configured
counter, each counter's share of the processor over the 1 s window
ending at the latest sample, recomputed with every sample batch: the
idle counters' summed share as the number in the donut's centre, a
legend of each wedge's label and share, and under the donut a stacked
strip of the shares of the last 30 s, one column per 100 ms. A counter
is a watched unsigned 32-bit count of cumulative microseconds carrying
a label and a role (`task`, `idle`, or `interrupt`), and the shares are

| Quantity | Value |
|----------|-------|
| $\Delta_i$ | The counter's wrapping (mod $2^{32}$) increase across the window |
| $T$ | $\sum \Delta_i$ over the `task` and `idle` counters |
| $I$ | $\sum \Delta_i$ over the `interrupt` counters |
| `interrupt` share | $\Delta_i / T$ |
| `task` or `idle` share | $(\Delta_i / T)\,(1 - I / T)$ |

with $T = 0$ showing no share and a waiting note.

Acceptance:

- Against counters advancing at known rates, each wedge's share matches
  the table within 0.2 points, the shares summing to 100 %, and the
  centre shows the idle share.
- A counter wrapping past $2^{32}$ inside the window keeps its share, in
  the donut and in every strip column.
- Before any sample the widget shows the waiting note and no share.

Covers:
- sys~obs_010~1

Needs: impl, test

### CPU load widget configuration
`app~views_021~1`

A CPU load widget shall hold its counters — path, label, role — as
workspace configuration: a new widget holding the preset below, every
counter watched at the 10 ms period, each counter's label and role
editable and the counter removable from the widget's menu, and a signal
dropped onto the widget joining as a `task` counter labelled with its
leaf name.

| Counter | Label | Role |
|---------|-------|------|
| `app_cpuLoad_data.taskTime_us[0]` | `other` | `task` |
| `app_cpuLoad_data.taskTime_us[1]` | `idle` | `idle` |
| `app_cpuLoad_data.taskTime_us[2]` | `1 ms` | `task` |
| `app_cpuLoad_data.taskTime_us[3]` | `10 ms` | `task` |
| `app_cpuLoad_data.taskTime_us[4]` | `200 ms` | `task` |
| `app_cpuLoad_data.taskTime_us[5]` | `usb` | `task` |
| `app_cpuLoad_data.taskTime_us[6]` | `server` | `task` |
| `HW_ADC_data.irqTime_us` | `cycle ISR` | `interrupt` |

Acceptance:

- A new widget lists the preset's eight counters, all watched at 10 ms.
- A label edit, a role change, and a removal made in the menu survive an
  app restart.
- A signal dropped onto the widget appears as a `task` wedge labelled
  with its leaf name.

Covers:
- sys~obs_010~1

Needs: impl, test
