---
status: draft
tags: [app, views]
---

# Telemetry view

### Telemetry view
`app~views_002~1`

The app shall display the fields of the most recently received
`Telemetry` message, updating as messages arrive.

Acceptance:

- After a message arrives with a changed field value, the view shows
  the new value; with no newer message, it holds the last.

Covers:
- sys~arch_002~1

Needs: impl, test
