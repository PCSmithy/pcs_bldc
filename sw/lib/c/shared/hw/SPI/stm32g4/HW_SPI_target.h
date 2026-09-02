#pragma once

// Target-specific half of HW_SPI; reached via HW_SPI.h.

/* Includes */
#include "lib_types.h"
#include "stm32g4xx_hal.h"

#include "HW_DMA.h"              // HW_DMA_channel_E (DMA-backed transfer mode)
#include "HW_SPI_channels.h"

/* Typedefs */

typedef struct
{
    bool enabled;
    SPI_HandleTypeDef hspi;

    HW_SPI_transferMode_E transferMode;
    HW_DMA_channel_E txDmaChannel;   // TX DMA stream; used when transferMode == DMA
} HW_SPI_busConfig_S;
