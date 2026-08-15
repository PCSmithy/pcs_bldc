---
status: draft
tags: [system, obs]
---

# Firmware build identity

### Firmware build identity
`sys~obs_003~1`

The firmware shall report over the protocol (`sys~conn_001~1`) a build
identity that differs between builds of differing source content.

Acceptance:

- Two builds of differing source content report differing build
  identities.
- The reported identity is identical across power cycles of the same
  image.

See also: [[identity-gate]]

Covers:

- (project goal: README.md, "Reference-quality embedded software
  development" + "Modeling, simulation, and observability
  infrastructure" — reproducible builds; live inspection bound to the
  running image.)

Needs: fw, test
