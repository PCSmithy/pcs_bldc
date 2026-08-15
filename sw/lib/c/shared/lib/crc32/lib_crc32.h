#pragma once

/* Includes */
#include "lib_types.h"

/* Public Function Declarations */

// IEEE 802.3 CRC-32 over len bytes (the Ethernet/zlib CRC).
uint32_t lib_crc32_compute(const uint8_t * const bytes, size_t len);
