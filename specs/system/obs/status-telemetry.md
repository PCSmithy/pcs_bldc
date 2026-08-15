---
status: draft
tags: [system, obs]
---

# Periodic status telemetry

### Periodic status telemetry
`sys~obs_001~1`

While a host holds the device's virtual serial port open
(`sys~arch_003~1`), the firmware shall publish a status message over the
protocol (`sys~conn_001~1`) reporting the operating mode, fault state,
bus voltage, bus current, and rotor velocity as of publication, at 10 Hz
within ± 2 % over any 10-second window.

Acceptance:

- A mode change or fault entry appears in the next status message
  published after it.

Covers:

- (project goal: README.md, "Modeling, simulation, and observability
  infrastructure" — rich USB-streamed telemetry at meaningful rates.)

Needs: fw, test
