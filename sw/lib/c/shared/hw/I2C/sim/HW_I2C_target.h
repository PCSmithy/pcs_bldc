#pragma once

// Target-specific half of HW_I2C; reached via HW_I2C.h.

/* Includes */
#include "lib_types.h"

/* Typedefs */

typedef struct
{
    bool enabled;
    HW_I2C_transferMode_E transferMode;
    uint32_t sclBitRateHz;
    char * busNameStr;
} HW_I2C_busConfig_S;
