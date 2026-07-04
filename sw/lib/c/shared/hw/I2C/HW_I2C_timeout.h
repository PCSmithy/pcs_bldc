#pragma once

/* Includes */
#include "lib_types.h"

/* Defines */

/* Typedefs */

/* Static Inline Functions */

/* Public Function Declarations */

// Per-transfer timeout in whole milliseconds for an I2C transfer of
// `numBytes` bytes at bit rate `fBitHz`:
//   t = ceil(1.1 * 9N / f_bit + 1ms)
// The 9 bits/byte accounts for the ACK clock alongside each data byte.
// Target-independent so the formula is unit-testable on the native target.
// Returns 0 when fBitHz is 0 (no meaningful bit rate).
uint32_t HW_I2C_computeTimeoutMs(uint32_t fBitHz, size_t numBytes);
