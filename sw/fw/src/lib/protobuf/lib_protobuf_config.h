#pragma once

/* Includes */
#include "lib_types.h"
// [impl->fw~conn_proto_001~1] pcs_Envelope: request_id + oneof of all
// protocol payloads, generated from sw/proto/pcs_bldc.proto.
#include "pcs_bldc.pb.h"

/* Defines */

// Worst-case encoded Envelope size, derived by nanopb from the schema's
// max_size options.
#define LIB_PROTOBUF_ENVELOPE_MAX  pcs_Envelope_size
