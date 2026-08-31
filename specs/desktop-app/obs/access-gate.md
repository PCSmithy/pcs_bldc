---
status: draft
tags: [app, obs]
---

# Build-identity gate

### Build-identity gate
`app~obs_002~1`

The app shall enable signal trace, signal write, and memory read only
while the device's reported build identity (`sys~obs_003~1`) equals
the build identity of the loaded firmware ELF from which signals are
resolved (`app~obs_001~1`), presenting the gated capabilities as
unavailable otherwise while requests and telemetry remain available.

Acceptance:

- With the running build's ELF loaded, a watch install
  (`app~obs_003~1`) is issued and succeeds.
- With a different build's ELF loaded, the gated capabilities are
  unavailable, while a ping round trip and the telemetry view
  (`app~views_002~1`) continue.

Covers:
- sys~obs_004~1

Needs: impl, test
