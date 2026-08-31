---
status: draft
tags: [system, ops]
---

# Command authority

### Command authority
`sys~ops_002~1`

Where the on-device controls and a host command the same requested
value — the drive mode (`sys~mc_005~1`) or the speed target — the
command most recently arriving at the motor-control application shall
determine it.

Acceptance:

- A host speed-target command following a dial adjustment leaves the
  host's value in effect, and a further dial adjustment leaves the
  dial's.
- A mode commanded by the host is superseded by a subsequent on-device
  mode input, and the reverse.

Covers:

- (project goal: README.md, "Reference-quality embedded software
  development" — source-blind command handling shared by the on-device
  controls and the host.)

Needs: fw, test
