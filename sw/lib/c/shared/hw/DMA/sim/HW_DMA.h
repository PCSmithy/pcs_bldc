#ifndef HW_DMA_H
#define HW_DMA_H

/* Includes */
#include "lib_types.h"

#include "HW_DMA_channels.h"   // project-provided HW_DMA_channel_E

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

typedef struct
{
    HW_DMA_direction_E direction;
    HW_DMA_width_E     width;      // width of one transferred item
    char *             channelNameStr;
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

#endif // HW_DMA_H
