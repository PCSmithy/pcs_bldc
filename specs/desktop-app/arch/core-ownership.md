---
status: draft
tags: [app, arch]
---

# Core ownership of device and firmware state

### Core ownership of device and firmware state
`app~arch_001~1`

The app shall hold the device session, protocol state, and the loaded
firmware ELF's debug data in its native core, independent of the UI
layer's lifecycle.

Acceptance:

- With a device connected and a watch list streaming, a UI reload
  reattaches the presentation to the same session: the stream renders
  from the already-installed watch list and the loaded firmware ELF's
  signals remain resolved.

Covers:
- sys~arch_002~1

Needs: impl, test
