/* Includes */
#include "HW_DMA.h"
#include "HW_DMA_sim.h"

/* Defines */

// Largest transfer the loopback model captures/injects (bytes). Tests stay well
// under this; longer transfers still move data through the caller's buffer, only
// the captured/injected copy is clamped.
#define HW_DMA_SIM_MAX_BYTES  (256U)

/* Typedefs */

typedef struct
{
    HW_DMA_completeCallback_F callback;
    void * callbackContext;
    HW_DMA_status_E status;

    // In-flight transfer, settled at HW_DMA_sim_tick().
    void *   memory;
    uint32_t numItems;
    bool     pending;

    // Memory-to-peripheral capture / peripheral-to-memory injection.
    uint8_t lastMem[HW_DMA_SIM_MAX_BYTES];
    size_t  lastMemLen;
    uint8_t injected[HW_DMA_SIM_MAX_BYTES];
    size_t  injectedLen;

    // Fault injection + inspection.
    bool     forceError;
    uint32_t transferCount;
} HW_DMA_channelData_S;

typedef struct
{
    const HW_DMA_config_S * config;
    bool initialized;

    HW_DMA_channelData_S channels[HW_DMA_CHANNEL_COUNT];
} HW_DMA_data_S;

/* Private Function Declarations */

static bool   HW_DMA_private_channelConfigValid(const HW_DMA_config_S * const config, HW_DMA_channel_E channel);
static void   HW_DMA_private_clearChannel(HW_DMA_channel_E channel);
static size_t HW_DMA_private_widthBytes(HW_DMA_width_E width);
static size_t HW_DMA_private_byteLength(HW_DMA_channel_E channel);

/* Private Data Definitions */

static HW_DMA_data_S HW_DMA_data;
static HW_DMA_data_S * const data = &HW_DMA_data;

/* Private Function Definitions */

static bool HW_DMA_private_channelConfigValid(const HW_DMA_config_S * const config, HW_DMA_channel_E channel)
{
    const HW_DMA_channelConfig_S * const cc = &config->channels[channel];
    const bool dirValid = (cc->direction == HW_DMA_DIRECTION_MEM_TO_PERIPH) ||
                          (cc->direction == HW_DMA_DIRECTION_PERIPH_TO_MEM);
    const bool widthValid = (cc->width == HW_DMA_WIDTH_8BIT) ||
                            (cc->width == HW_DMA_WIDTH_16BIT) ||
                            (cc->width == HW_DMA_WIDTH_32BIT);
    return (dirValid && widthValid);
}

static void HW_DMA_private_clearChannel(HW_DMA_channel_E channel)
{
    HW_DMA_channelData_S * const cd = &data->channels[channel];
    cd->callback        = NULL;
    cd->callbackContext = NULL;
    cd->status          = HW_DMA_STATUS_IDLE;
    cd->memory          = NULL;
    cd->numItems        = 0U;
    cd->pending         = false;
    cd->lastMemLen      = 0U;
    cd->injectedLen     = 0U;
    cd->forceError      = false;
    cd->transferCount   = 0U;
}

static size_t HW_DMA_private_widthBytes(HW_DMA_width_E width)
{
    size_t bytes = 1U;
    switch (width)
    {
        case HW_DMA_WIDTH_8BIT:  bytes = 1U; break;
        case HW_DMA_WIDTH_16BIT: bytes = 2U; break;
        case HW_DMA_WIDTH_32BIT: bytes = 4U; break;
        default:                 bytes = 1U; break;
    }
    return bytes;
}

static size_t HW_DMA_private_byteLength(HW_DMA_channel_E channel)
{
    const HW_DMA_channelData_S * const cd = &data->channels[channel];
    const HW_DMA_width_E width = data->config->channels[channel].width;
    return (size_t)cd->numItems * HW_DMA_private_widthBytes(width);
}

/* Public Function Definitions */

// [impl->fw~hal_dma_001~1]
bool HW_DMA_init(const HW_DMA_config_S * const config)
{
    bool ret = false;
    if ((config != NULL) && (config->channels != NULL) && (config->numChannels <= HW_DMA_CHANNEL_COUNT))
    {
        bool success = true;
        for (HW_DMA_channel_E channel = 0U; channel < HW_DMA_CHANNEL_COUNT; channel++)
        {
            success &= HW_DMA_private_channelConfigValid(config, channel);
        }

        if (success)
        {
            data->config = config;
            for (HW_DMA_channel_E channel = 0U; channel < HW_DMA_CHANNEL_COUNT; channel++)
            {
                HW_DMA_private_clearChannel(channel);
            }
            data->initialized = true;
            ret = true;
        }
    }
    return ret;
}

// [impl->fw~hal_dma_002~1]
bool HW_DMA_startTransfer(HW_DMA_channel_E channel, void * memory, uint32_t numItems)
{
    bool ret = false;
    if ((data->initialized) &&
        (channel < HW_DMA_CHANNEL_COUNT) &&
        (memory != NULL) &&
        (numItems > 0U))
    {
        HW_DMA_channelData_S * const cd = &data->channels[channel];
        cd->memory   = memory;
        cd->numItems = numItems;
        cd->status   = HW_DMA_STATUS_BUSY;
        cd->pending  = true;

        // Memory-to-peripheral: capture the bytes leaving memory now (the
        // engine reads them during the transfer). Peripheral-to-memory fills
        // the buffer at completion (HW_DMA_sim_tick).
        if (data->config->channels[channel].direction == HW_DMA_DIRECTION_MEM_TO_PERIPH)
        {
            const size_t byteLen = HW_DMA_private_byteLength(channel);
            const size_t copyLen = (byteLen < HW_DMA_SIM_MAX_BYTES) ? byteLen : HW_DMA_SIM_MAX_BYTES;
            const uint8_t * const src = (const uint8_t *)memory;
            for (size_t i = 0U; i < copyLen; i++)
            {
                cd->lastMem[i] = src[i];
            }
            cd->lastMemLen = byteLen;
        }
        ret = true;
    }
    return ret;
}

// [impl->fw~hal_dma_003~1]
HW_DMA_status_E HW_DMA_getStatus(HW_DMA_channel_E channel)
{
    HW_DMA_status_E status = HW_DMA_STATUS_ERROR;
    if ((data->initialized) && (channel < HW_DMA_CHANNEL_COUNT))
    {
        status = data->channels[channel].status;
    }
    return status;
}

// [impl->fw~hal_dma_003~1]
bool HW_DMA_registerCallback(HW_DMA_channel_E channel, HW_DMA_completeCallback_F callback, void * context)
{
    bool ret = false;
    if ((data->initialized) && (channel < HW_DMA_CHANNEL_COUNT))
    {
        data->channels[channel].callback        = callback;
        data->channels[channel].callbackContext = context;
        ret = true;
    }
    return ret;
}

/* SIL control + inspection (HW_DMA_sim.h) */

void HW_DMA_sim_reset(void)
{
    for (HW_DMA_channel_E channel = 0U; channel < HW_DMA_CHANNEL_COUNT; channel++)
    {
        HW_DMA_private_clearChannel(channel);
    }
}

// [impl->fw~hal_dma_003~1]
void HW_DMA_sim_tick(void)
{
    if (data->initialized)
    {
        for (HW_DMA_channel_E channel = 0U; channel < HW_DMA_CHANNEL_COUNT; channel++)
        {
            HW_DMA_channelData_S * const cd = &data->channels[channel];
            if (cd->pending)
            {
                cd->pending = false;
                if (cd->forceError)
                {
                    cd->status = HW_DMA_STATUS_ERROR;
                }
                else
                {
                    if (data->config->channels[channel].direction == HW_DMA_DIRECTION_PERIPH_TO_MEM)
                    {
                        const size_t byteLen = HW_DMA_private_byteLength(channel);
                        uint8_t * const dst = (uint8_t *)cd->memory;
                        for (size_t i = 0U; i < byteLen; i++)
                        {
                            dst[i] = (i < cd->injectedLen) ? cd->injected[i] : 0U;
                        }
                    }
                    cd->status = HW_DMA_STATUS_COMPLETE;
                }
                cd->transferCount++;

                if (cd->callback != NULL)
                {
                    cd->callback(channel, cd->callbackContext);
                }
            }
        }
    }
}

void HW_DMA_sim_setInjectedPeriphData(HW_DMA_channel_E channel, const uint8_t * data_, size_t length)
{
    if ((channel < HW_DMA_CHANNEL_COUNT) && (data_ != NULL))
    {
        HW_DMA_channelData_S * const cd = &data->channels[channel];
        const size_t copyLen = (length < HW_DMA_SIM_MAX_BYTES) ? length : HW_DMA_SIM_MAX_BYTES;
        for (size_t i = 0U; i < copyLen; i++)
        {
            cd->injected[i] = data_[i];
        }
        cd->injectedLen = copyLen;
    }
}

size_t HW_DMA_sim_getLastMemoryData(HW_DMA_channel_E channel, uint8_t * out, size_t maxLength)
{
    size_t memLen = 0U;
    if ((channel < HW_DMA_CHANNEL_COUNT) && (out != NULL))
    {
        const HW_DMA_channelData_S * const cd = &data->channels[channel];
        memLen = cd->lastMemLen;
        const size_t captured = (cd->lastMemLen < HW_DMA_SIM_MAX_BYTES) ? cd->lastMemLen : HW_DMA_SIM_MAX_BYTES;
        const size_t copyLen  = (captured < maxLength) ? captured : maxLength;
        for (size_t i = 0U; i < copyLen; i++)
        {
            out[i] = cd->lastMem[i];
        }
    }
    return memLen;
}

void HW_DMA_sim_setForceError(HW_DMA_channel_E channel, bool forceError)
{
    if (channel < HW_DMA_CHANNEL_COUNT)
    {
        data->channels[channel].forceError = forceError;
    }
}

uint32_t HW_DMA_sim_getTransferCount(HW_DMA_channel_E channel)
{
    uint32_t count = 0U;
    if (channel < HW_DMA_CHANNEL_COUNT)
    {
        count = data->channels[channel].transferCount;
    }
    return count;
}
