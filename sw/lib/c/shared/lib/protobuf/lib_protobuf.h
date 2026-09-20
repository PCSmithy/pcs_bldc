#pragma once

/* Includes */
#include "lib_types.h"
#include "pb.h"   // pb_msgdesc_t

/* Defines */

// Envelope header worst case, the room lib_protobuf_encodeEnvelope reserves in
// front of the payload: request_id key+varint (6) and payload key (5) + length (5).
#define LIB_PROTOBUF_ENVELOPE_HEADER_MAX (16U)

/* Public Function Declarations */

// Encode message (described by fields) into buffer; false when the encoding
// fails or exceeds bufferLen.
bool lib_protobuf_encode(const pb_msgdesc_t * const fields, const void * const message,
                         uint8_t * const buffer, size_t bufferLen,
                         size_t * const encodedLen);

// Encode an envelope around one payload without walking the envelope's oneof:
// request_id (field 1, omitted when 0), then the payload as the
// length-delimited field `payloadTag`. Byte-identical to encoding the full
// envelope, at the cost of the payload alone. False when it exceeds bufferLen.
bool lib_protobuf_encodeEnvelope(uint32_t requestId, uint32_t payloadTag,
                                 const pb_msgdesc_t * const payloadFields, const void * const payload,
                                 uint8_t * const buffer, size_t bufferLen,
                                 size_t * const encodedLen);

// Decode len bytes into message (described by fields); false when the bytes
// do not decode as that message type.
bool lib_protobuf_decode(const pb_msgdesc_t * const fields,
                         const uint8_t * const bytes, size_t len,
                         void * const message);
