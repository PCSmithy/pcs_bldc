---
status: draft
tags: [app, arch]
---

# Session restore

### Session restore
`app~arch_002~1`

The app shall restore its saved session context at launch per:

| Context | Restored |
|---------|----------|
| Serial port | The session re-opens on the saved port when present (`app~conn_001~1`); port selection is offered otherwise |
| Firmware ELF | The saved path reloads (`app~obs_001~1`) when readable; the load action is offered otherwise |
| Watch list | Reinstalled (`app~obs_003~1`) after connecting, while the device's and the loaded ELF's build identities are equal (`app~obs_002~1`); held otherwise |

Acceptance:

- Relaunching with the board present and the saved ELF readable
  reconnects, reloads, and reinstalls the prior watch list without
  user action.
- Relaunching with the saved port absent offers port selection with
  the ELF still reloaded; relaunching with the ELF unreadable offers
  the load action with the port still connected.

See also: [[core-ownership]], [[../views/workspace|workspace]]

Covers:
- sys~arch_002~1

Needs: impl, test
