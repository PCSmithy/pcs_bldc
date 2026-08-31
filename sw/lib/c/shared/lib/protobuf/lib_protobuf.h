#pragma once

/* Includes */
#include "lib_types.h"
#include "pb.h"   // pb_msgdesc_t

/* Public Function Declarations */

// Encode message (described by fields) into buffer; false when the encoding
// fails or exceeds bufferLen.
bool lib_protobuf_encode(const pb_msgdesc_t * const fields, const void * const message,
                         uint8_t * const buffer, size_t bufferLen,
                         size_t * const encodedLen);

// Decode len bytes into message (described by fields); false when the bytes
// do not decode as that message type.
bool lib_protobuf_decode(const pb_msgdesc_t * const fields,
                         const uint8_t * const bytes, size_t len,
                         void * const message);
