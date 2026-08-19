---
status: draft
tags: [firmware, conn, server]
---

# Request server

The app_server module: envelope dispatch over the frame driver
([[proto]]), answering each received envelope with a correlated reply,
applying board commands to the shared motor-control request state, and
serving the trace services ([[trace]]) — watch streaming, memory read
and write, and the trace capability report.

### Request acknowledgement
`fw~conn_server_001~1`

The firmware shall answer every received envelope
(`fw~conn_proto_001~1`) with a reply envelope carrying the received
`request_id`, whose payload follows:

| Received envelope | Reply payload |
|-------------------|---------------|
| A request producing a result | The result payload (e.g. `Identity` for `IdentityRequest`) |
| A request succeeding without a result | `Response` with `accepted` set |
| A rejected request, or a payload that is not a recognized request | `Response` with `accepted` clear and `cause` naming the reason |

Acceptance:
- A `PingRequest` receives a `Response` with `accepted` set, carrying
  the request's `request_id`.
- An envelope bearing no recognized request payload receives a
  `Response` with `accepted` clear and a non-empty `cause`.

Covers:
- sys~conn_003~1

Needs: impl, test

## Board commands

`board.Request` commands act on the request state shared with the
on-device controls (`sys~ops_002~1`). A `board.Mode` value is either
`MODE_OFF` — the bridge-disable request of `fw~mc_006~1` — or names a
commutation method (`sys~mc_005~1`).

### SetMode handling
`fw~conn_server_002~1`

An accepted `SetMode` shall request the commanded state of the motor
control application — `MODE_OFF` the bridge disabled, a method value
the bridge enabled running that method — rejecting the command when:

| Rejected when |
|---------------|
| The value names no `board.Mode` member |
| A method other than the active one is named while the bridge is enabled (`sys~mc_005~1`) |
| A method is named while a fault is latched (`fw~mc_007~1`) |

Acceptance:
- With the bridge disabled and no fault latched, a method value is
  accepted and the bridge enable is requested with that method;
  `MODE_OFF` is accepted in every state.

Covers:
- sys~ops_002~1

Needs: impl, test

### SetVelocity handling
`fw~conn_server_003~1`

An accepted `SetVelocity` shall become the shared speed target
(`sys~ops_002~1`), rejecting the command when the value is not finite
or its magnitude exceeds the configured maximum (`fw~mc_008~1`).

Acceptance:
- A finite value within the maximum becomes the speed target.
- A non-finite value and an over-maximum value are each rejected, the
  target unchanged.

Covers:
- sys~ops_002~1

Needs: impl, test

### ClearFault handling
`fw~conn_server_004~1`

An accepted `ClearFault` shall perform the fault-clear action
(`fw~safety_001~1`, `fw~safety_002~1`), the command accepted with or
without a fault latched.

Acceptance:
- With a fault latched, an accepted `ClearFault` releases it.
- With no fault latched, `ClearFault` is accepted.

Covers:
- sys~safety_001~1

Needs: impl, test
