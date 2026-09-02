#pragma once

// Target-specific half of HW_SPI; reached via HW_SPI.h.

/* Includes */
#include "lib_types.h"

#include "HW_SPI_channels.h"

/* Typedefs */

typedef struct
{
    bool enabled;
    HW_SPI_transferMode_E transferMode;
    char * busNameStr;
} HW_SPI_busConfig_S;
