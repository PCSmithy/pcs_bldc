#pragma once

/* Includes */
#include "lib_types.h"
// Decode-only messages whose lists stream through callbacks: zero sizes keep
// nanopb's shared_Envelope_size defined as the bound of what the board encodes.
#define trace_WatchRequest_size 0
#define board_Request_size 0

// [impl->fw~conn_proto_001~1] shared_Envelope, generated from the framework
// schema plus this board's sw/proto/board.proto extension payloads.
#include "shared.pb.h"

/* Defines */

// Worst-case encoded Envelope size, derived by nanopb from the schemas'
// max_size options.
#define LIB_PROTOBUF_ENVELOPE_MAX  shared_Envelope_size
