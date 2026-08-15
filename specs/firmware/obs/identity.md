---
status: draft
tags: [firmware, obs, identity]
---

# Build identity

The firmware's build identity: a string derived by the build from the
git state of the source tree, embedded in the image, and reported over
the protocol on request.

See also: the system spec `sys~obs_003~1`; [[proto]] (the envelope
carrying the request).

### Identity content
`fw~obs_identity_001~1`

The firmware build shall embed in the image an identity string derived
from the built source tree per:

| Tree state | Identity string |
|------------|-----------------|
| Working tree matches the checked-out commit | The commit hash abbreviated to 12 hex digits (`git rev-parse --short=12 HEAD`) |
| Working tree differs from the checked-out commit | The abbreviated commit hash, `+`, and the first 8 hex digits of the SHA-1 of the `git diff HEAD` output |

Acceptance:
- A clean-tree build embeds exactly the output of
  `git rev-parse --short=12 HEAD`.
- A dirty-tree build embeds the abbreviated commit hash, `+`, and 8 hex
  digits, and differs from the clean-tree build of the same commit.
- Rebuilding an unchanged tree embeds an identical identity; editing a
  tracked source file and rebuilding embeds a differing one.

Covers:
- sys~obs_003~1

Needs: impl, test

### Identity query
`fw~obs_identity_002~1`

The firmware shall answer a received `IdentityRequest` payload
(`fw~conn_proto_001~1`) with an `Identity` payload carrying the identity
string embedded per `fw~obs_identity_001~1`.

Acceptance:
- The `Identity` response to an `IdentityRequest` carries the identity
  computed from the built tree per `fw~obs_identity_001~1`.

Covers:
- sys~obs_003~1

Needs: impl, test
