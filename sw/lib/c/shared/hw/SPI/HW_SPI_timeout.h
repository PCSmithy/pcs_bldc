#ifndef HW_SPI_TIMEOUT_H
#define HW_SPI_TIMEOUT_H

/* Includes */
#include "lib_types.h"

/* Defines */

/* Typedefs */

/* Static Inline Functions */

/* Public Function Declarations */

// Per-transfer timeout in whole milliseconds for a software (polled)
// transfer of `numBytes` bytes at bit rate `fBitHz`:
//   t = ceil(1.1 * 8N / f_bit + 1ms)
// Target-independent so the formula is unit-testable on the native target.
// Returns 0 when fBitHz is 0 (no meaningful bit rate).
uint32_t HW_SPI_computeTimeoutMs(uint32_t fBitHz, size_t numBytes);

#endif // HW_SPI_TIMEOUT_H
