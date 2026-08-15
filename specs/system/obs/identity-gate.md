---
status: draft
tags: [system, obs]
---

# Build-identity gate on signal access

### Build-identity gate on signal access
`sys~obs_004~1`

The desktop app shall enable signal trace (`sys~obs_005~1`) and signal
write (`sys~obs_006~1`) only while the firmware's reported build
identity (`sys~obs_003~1`) matches the build identity of the ELF from
which signals are resolved (`sys~obs_002~1`).

Acceptance:

- With the ELF of the running build loaded, trace and write requests
  succeed.
- With the ELF of a different build loaded, the app presents trace and
  write as unavailable, while requests (`sys~conn_003~1`) and status
  telemetry (`sys~obs_001~1`) remain available.

See also: [[firmware-identity]], [[signal-selection]]

Covers:

- (project goal: README.md, "Modeling, simulation, and observability
  infrastructure" — live inspection bound to the running image.)

Needs: app, test
