/* Includes */
#include "HW_SPI_timeout.h"

/* Defines */

// 1.1 margin × 8 bits/byte × 1000 ms/s, folded into one integer constant
// so the per-byte transfer time scales straight to milliseconds.
#define HW_SPI_TIMEOUT_NUMERATOR_PER_BYTE    (8800U)

/* Typedefs */

/* Private Function Declarations */

/* Private Data Definitions */

/* Private Function Definitions */

/* Public Function Definitions */

// [impl->fw~hal_spi_003~1]
uint32_t HW_SPI_computeTimeoutMs(uint32_t fBitHz, size_t numBytes)
{
    uint32_t timeoutMs = 0U;
    if (fBitHz > 0U)
    {
        // ceil(8800·N / f_bit) + 1ms, in uint64 to avoid overflow on the
        // numerator for large transfers. The +1ms is exact, so it adds
        // outside the ceiling.
        const uint64_t numerator = (uint64_t)HW_SPI_TIMEOUT_NUMERATOR_PER_BYTE * (uint64_t)numBytes;
        const uint64_t ceilDiv   = (numerator + (uint64_t)fBitHz - 1U) / (uint64_t)fBitHz;
        timeoutMs = (uint32_t)(ceilDiv + 1U);
    }
    return timeoutMs;
}
