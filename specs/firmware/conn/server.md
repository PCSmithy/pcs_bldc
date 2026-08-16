---
status: draft
tags: [firmware, conn, server]
---

# Request server

The app_server module: envelope dispatch over the frame driver
([[proto]]), answering each received envelope with a correlated reply.

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
