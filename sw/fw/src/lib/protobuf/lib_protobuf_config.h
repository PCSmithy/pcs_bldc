#pragma once

/* Includes */
#include "lib_types.h"
// [impl->fw~conn_proto_001~1] shared_Envelope: request_id + oneof of all
// protocol payloads. Generated from the framework schema
// (sw/lib/c/shared/proto/shared.proto), which imports this board's
// sw/proto/board.proto for the two fixed-name extension payloads.
#include "shared.pb.h"

/* Defines */

// Worst-case encoded Envelope size, derived by nanopb from the schemas'
// max_size options.
#define LIB_PROTOBUF_ENVELOPE_MAX  shared_Envelope_size
