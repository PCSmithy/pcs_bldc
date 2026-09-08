#pragma once

// Target-specific half of HW_DMA; reached via HW_DMA.h.

/* Includes */
#include "lib_types.h"
#include "stm32g4xx_hal.h"

#include "HW_DMA_channels.h"   // project-provided HW_DMA_channel_E

/* Typedefs */

// Per-channel configuration. The HAL handle carries the target-specific
// transfer setup (Instance, DMAMUX Request, direction, data widths, increment
// modes, mode, priority); periphAddress is the peripheral data register the
// channel moves data to/from; irqn is the NVIC line for the channel's DMA
// interrupt.
typedef struct
{
    DMA_HandleTypeDef hdma;
    uint32_t          periphAddress;
    IRQn_Type         irqn;
} HW_DMA_channelConfig_S;

/* Public Function Declarations */

// Service a channel's DMA interrupt: forwards to the HAL, which drives the
// registered completion/error callbacks. Call from the board's DMA IRQ handler
// for the channel. NVIC enable + the board DMA IRQ handlers are wired with the
// SPI-DMA integration, where the path is bench-verified.
void HW_DMA_irqHandler(HW_DMA_channel_E channel);
