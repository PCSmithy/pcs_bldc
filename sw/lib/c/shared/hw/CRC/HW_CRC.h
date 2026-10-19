#pragma once

// IEEE 802.3 CRC-32 (the Ethernet/zlib CRC) from the CRC unit on the target,
// lib_crc32 in the sim. Single-instance, one user at a time: the protocol
// server's frame path.

/* Includes */
#include "lib_types.h"

/* Target Config */
#include "HW_CRC_target.h"   // HW_CRC_config_S

#ifdef __cplusplus
extern "C" {
#endif

/* Public Function Declarations */
bool HW_CRC_init(const HW_CRC_config_S * const config);

// CRC-32 over len bytes; zero before init.
uint32_t HW_CRC_compute32(const uint8_t * const bytes, size_t len);

#ifdef __cplusplus
}
#endif
