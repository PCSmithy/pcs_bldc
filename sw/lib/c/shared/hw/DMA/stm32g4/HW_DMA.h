#ifndef HW_DMA_H
#define HW_DMA_H

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

typedef struct
{
    const HW_DMA_channelConfig_S * channels;
    size_t numChannels;
} HW_DMA_config_S;

// Per-channel transfer status. A transfer passes through BUSY until its
// completion is signalled COMPLETE or ERROR.
typedef enum
{
    HW_DMA_STATUS_IDLE,
    HW_DMA_STATUS_BUSY,
    HW_DMA_STATUS_COMPLETE,
    HW_DMA_STATUS_ERROR,
} HW_DMA_status_E;

// Invoked once at completion of a transfer. `context` is the pointer supplied
// to HW_DMA_registerCallback.
typedef void (*HW_DMA_completeCallback_F)(HW_DMA_channel_E channel, void * context);

/* Public Function Declarations */

bool HW_DMA_init(const HW_DMA_config_S * const config);

// Start a non-blocking transfer of numItems items between `memory` and the
// channel's peripheral, in the channel's configured direction and width.
// Returns false (and starts nothing) if uninitialized, the channel is out of
// range, memory is NULL, or numItems is zero.
bool HW_DMA_startTransfer(HW_DMA_channel_E channel, void * memory, uint32_t numItems);

HW_DMA_status_E HW_DMA_getStatus(HW_DMA_channel_E channel);
bool HW_DMA_registerCallback(HW_DMA_channel_E channel, HW_DMA_completeCallback_F callback, void * context);

// Service a channel's DMA interrupt: forwards to the HAL, which drives the
// registered completion/error callbacks. Call from the board's DMA IRQ handler
// for the channel. NVIC enable + the board DMA IRQ handlers are wired with the
// SPI-DMA integration (M4), where the path is bench-verified.
void HW_DMA_irqHandler(HW_DMA_channel_E channel);

#endif // HW_DMA_H
