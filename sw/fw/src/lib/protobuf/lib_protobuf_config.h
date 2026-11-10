#pragma once

/* Includes */
#include "lib_types.h"
// The board only DECODES trace.WatchRequest and board.Request (their repeated
// lists — the watches, a Command's setpoints — stream through nanopb
// callbacks into the server's and the sequencer's own tables), so they
// contribute nothing to the envelope's ENCODE bound; defining their sizes as
// zero keeps nanopb's shared_Envelope_size defined for everything the board
// transmits.
#define trace_WatchRequest_size 0
#define board_Request_size 0

// [impl->fw~conn_proto_001~1] shared_Envelope, generated from the framework
// schema plus this board's sw/proto/board.proto extension payloads.
#include "shared.pb.h"

/* Defines */

// Worst-case encoded Envelope size, derived by nanopb from the schemas'
// max_size options.
#define LIB_PROTOBUF_ENVELOPE_MAX  shared_Envelope_size
