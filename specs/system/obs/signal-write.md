---
status: draft
tags: [system, obs]
---

# Signal write

### Signal write
`sys~obs_006~1`

The firmware shall write a host-supplied value of up to 8 bytes to a
host-specified memory location (`sys~obs_002~1`) once per request,
firmware readers observing the location's prior contents or the written
value in full.

Acceptance:

- A written variable subsequently traced (`sys~obs_005~1`) returns the
  written value.
- In a SIL scenario writing a multi-byte variable the firmware reads
  every millisecond, every read returns the prior value or the written
  value in full.

See also: [[signal-selection]], [[identity-gate]]

Covers:

- (project goal: README.md, "Modeling, simulation, and observability
  infrastructure" — live inspection, command, and tuning via the desktop
  visualizer / control app.)

Needs: fw, test
