#pragma once

/* Includes */
#include "lib_types.h"
// The board only DECODES trace.WatchRequest (its repeated watches stream
// through a nanopb callback into the server's own table), so it contributes
// nothing to the envelope's ENCODE bound; defining its size as zero keeps
// nanopb's shared_Envelope_size defined for everything the board transmits.
#define trace_WatchRequest_size 0

// [impl->fw~conn_proto_001~1] shared_Envelope, generated from the framework
// schema plus this board's sw/proto/board.proto extension payloads.
#include "shared.pb.h"

/* Defines */

// Worst-case encoded Envelope size, derived by nanopb from the schemas'
// max_size options.
#define LIB_PROTOBUF_ENVELOPE_MAX  shared_Envelope_size
