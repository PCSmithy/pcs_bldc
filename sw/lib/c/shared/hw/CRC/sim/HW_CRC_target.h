#pragma once

// Target-specific half of HW_CRC; reached via HW_CRC.h.

/* Includes */
#include "lib_types.h"

/* Typedefs */

// The sim computes with lib_crc32, so there is nothing to configure.
typedef struct
{
    uint8_t _void;
} HW_CRC_config_S;
