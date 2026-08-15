#pragma once

/* Includes */
#include "lib_types.h"

/* Defines */

// Worst-case COBS-encoded size for a payload of len bytes: one code byte per
// started 254-byte block, plus one for the (possibly empty) first block.
#define LIB_COBS_ENCODED_MAX(len)  ((len) + 1U + ((len) / 254U))

/* Public Function Declarations */

// COBS-encode len bytes (delimiters are the caller's concern); false when
// outputMax is below LIB_COBS_ENCODED_MAX(len).
bool lib_cobs_encode(const uint8_t * const input, size_t len,
                     uint8_t * const output, size_t outputMax,
                     size_t * const encodedLen);

// Decode one COBS block (no delimiters); false on malformed input or decoded
// data exceeding outputMax.
bool lib_cobs_decode(const uint8_t * const input, size_t len,
                     uint8_t * const output, size_t outputMax,
                     size_t * const decodedLen);
