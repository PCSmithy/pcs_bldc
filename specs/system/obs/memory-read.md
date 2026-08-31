---
status: draft
tags: [system, obs]
---

# Memory read

### Memory read
`sys~obs_008~1`

The firmware shall report, on request over the protocol
(`sys~conn_001~1`), the current contents of a host-specified memory
span of up to 128 bytes, rejecting a span outside its readable memory.

Acceptance:

- A read of the span at a firmware variable's address and size returns
  that variable's current value.

See also: [[signal-selection]], [[signal-write]], [[identity-gate]]

Covers:

- (project goal: README.md, "Modeling, simulation, and observability
  infrastructure" — live inspection, command, and tuning.)

Needs: fw, test
