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

typedef struct
{
    HW_SPI_bus_E bus;
    HW_SPI_chipSelectMode_E csMode;
    HW_SPI_csGpioConfig_S csGpioConfig; // ignored if csMode != GPIO
    char * channelNameStr;
} HW_SPI_channelConfig_S;
