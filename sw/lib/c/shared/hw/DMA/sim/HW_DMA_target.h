#pragma once

// Target-specific half of HW_DMA; reached via HW_DMA.h.

/* Includes */
#include "lib_types.h"

/* Typedefs */

// Transfer direction between a memory buffer and the channel's peripheral.
typedef enum
{
    HW_DMA_DIRECTION_MEM_TO_PERIPH,
    HW_DMA_DIRECTION_PERIPH_TO_MEM,
} HW_DMA_direction_E;

// Width of one transferred data item.
typedef enum
{
    HW_DMA_WIDTH_8BIT,
    HW_DMA_WIDTH_16BIT,
    HW_DMA_WIDTH_32BIT,
} HW_DMA_width_E;

// Per-channel configuration. Direction and item width are explicit (the
// stm32g4 target carries them inside its HAL handle).
typedef struct
{
    HW_DMA_direction_E direction;
    HW_DMA_width_E     width;      // width of one transferred item
    char *             channelNameStr;
} HW_DMA_channelConfig_S;
