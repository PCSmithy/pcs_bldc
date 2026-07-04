---
status: draft
tags: [system, pd]
---

# USB-PD sink status monitoring

### USB-PD sink status monitoring
`sys~pd_001~1`

The firmware shall make the USB-PD sink controller's power-delivery status
available to firmware consumers: whether an explicit PD contract is active, the
negotiated contract voltage and current, the live VBUS voltage, the USB Type-C
connection status, and whether the controller is present and responding.

Acceptance:
- The exposed contract-active flag, negotiated voltage, and negotiated current
  match the PD contract in force.
- The exposed VBUS voltage matches the VBUS rail within ±0.1 V, and the exposed
  Type-C connection status matches the physical attach state.
- The exposed reachability status reports the controller as available when it
  responds and as unavailable when it does not.

Covers:
- (project goal: README.md, "Modeling, simulation, and observability
  infrastructure" — live diagnostic visibility into the device's power
  subsystem.)

Needs: fw, test
