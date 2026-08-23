---
status: draft
tags: [app, views]
---

# Value table

### Value table
`app~views_006~1`

A table widget shall display one row per signal added to it — the
signal's name, its value per the mode below, rendered per
the value rendering table (`app~views_013~1`), its scalar type, and
its sample period:

| Mode | Value shown |
|------|-------------|
| No cursor time held | The latest streamed value, updating as samples arrive |
| A cursor time held (`app~views_005~1`) | The value at the cursor time, its absence stated for a time inside a tick-count gap |

Acceptance:

- A signal added to the table shows its newest streamed value beside
  the type and period it was resolved with.
- With a cursor time held (`app~views_005~1`), each row shows the
  value at that time.

See also: [[cursor]], [[workspace]]

Covers:
- sys~arch_002~1

Needs: impl, test
