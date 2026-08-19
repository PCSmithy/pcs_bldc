---
status: draft
tags: [app, conn]
---

# Serial session

### Serial session
`app~conn_001~1`

The app shall open a user-selected serial port as the device session,
hold it while the port exists, and re-open it when the port disappears
and returns.

Acceptance:

- A selected port opens and carries a request/reply round trip
  (`app~conn_003~1`).
- After the port disappears and re-enumerates (a device reset), the
  session re-opens without user action.

Covers:
- sys~arch_003~1

Needs: impl, test
