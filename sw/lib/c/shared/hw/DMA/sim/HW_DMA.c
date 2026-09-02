/* Includes */
#include "HW_DMA.h"
#include "HW_DMA_simData.h"
#include "SIL_irq.h"

/* Defines */

// Completion dispatch rides the peripheral-ISR rung of the sim NVIC ladder
// (docs/sil/sim-interrupts.md), alongside sim HW_USB.
#define HW_DMA_IRQ_PRIORITY   (8U)

/* Private Function Declarations */

static bool   HW_DMA_private_channelConfigValid(const HW_DMA_config_S * const config, HW_DMA_channel_E channel);
static size_t HW_DMA_private_widthBytes(HW_DMA_width_E width);
static size_t HW_DMA_private_byteLength(HW_DMA_channel_E channel);
// External linkage (the HW_ADC_sim_completionDispatch pattern): SIL scenarios
// resolve the completion ISR by name, which -O2 strips from a static.
void HW_DMA_sim_completionDispatch(void);

/* Private Data Definitions */

HW_DMA_data_S HW_DMA_data;
static HW_DMA_data_S * const data = &HW_DMA_data;

// The completion service's framework handle. Lives outside HW_DMA_data so the
// re-entrant init's clean slate can still cancel the previous registration.
static int32_t HW_DMA_completionIrqHandle = SIL_IRQ_HANDLE_INVALID;

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

// [impl->fw~hal_dma_003~1]
// Completion interrupt: pended by HW_DMA_startTransfer, dispatched in the same
// step's ISR phase. The pending set is snapshotted at entry so a transfer started
// from a callback settles in the next interrupt, as hardware does.
void HW_DMA_sim_completionDispatch(void)
{
    if (data->initialized)
    {
        uint32_t due = 0U;
        for (HW_DMA_channel_E channel = 0U; channel < HW_DMA_CHANNEL_COUNT; channel++)
        {
            if (data->channels[channel].pending)
            {
                data->channels[channel].pending = false;
                due |= (1UL << channel);
            }
        }

        for (HW_DMA_channel_E channel = 0U; channel < HW_DMA_CHANNEL_COUNT; channel++)
        {
            HW_DMA_channelData_S * const cd = &data->channels[channel];
            if ((due & (1UL << channel)) != 0U)
            {
                if (cd->forceError)
                {
                    cd->status = HW_DMA_STATUS_ERROR;
                }
                else
                {
                    // memory is never NULL from startTransfer; a SIL-fabricated
                    // transfer may leave it unset.
                    if ((data->config->channels[channel].direction == HW_DMA_DIRECTION_PERIPH_TO_MEM) &&
                        (cd->memory != NULL))
                    {
                        const size_t byteLen = HW_DMA_private_byteLength(channel);
                        uint8_t * const dst = (uint8_t *)cd->memory;
                        for (size_t i = 0U; i < byteLen; i++)
                        {
                            dst[i] = (uint8_t)i;
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

/* Public Function Definitions */

// [impl->fw~hal_dma_001~1]
bool HW_DMA_init(const HW_DMA_config_S * const config)
{
    bool ret = false;

    // Re-entrant: every call, accepted or rejected, is a clean slate; the
    // completion service goes with it.
    SIL_irq_cancel(HW_DMA_completionIrqHandle);
    HW_DMA_completionIrqHandle = SIL_IRQ_HANDLE_INVALID;
    *data = (HW_DMA_data_S){ 0 };

    if ((config != NULL) && (config->channels != NULL) && (config->numChannels <= HW_DMA_CHANNEL_COUNT))
    {
        bool valid = true;
        for (HW_DMA_channel_E channel = 0U; channel < HW_DMA_CHANNEL_COUNT; channel++)
        {
            valid &= HW_DMA_private_channelConfigValid(config, channel);
        }

        if (valid)
        {
            data->config      = config;
            data->initialized = true;
            HW_DMA_completionIrqHandle = SIL_irq_registerPended(HW_DMA_sim_completionDispatch,
                                                                HW_DMA_IRQ_PRIORITY);
            ret = true;
        }
    }
    return ret;
}

// [impl->fw~hal_dma_002~1]
bool HW_DMA_startTransfer(HW_DMA_channel_E channel, void * memory, uint32_t numItems)
{
    bool ret = false;
    // A start on an in-flight channel is refused, as HAL_BUSY would: overwriting
    // the transfer would fold its pending completion into the new one.
    if ((data->initialized) &&
        (channel < HW_DMA_CHANNEL_COUNT) &&
        (!data->channels[channel].pending) &&
        (memory != NULL) &&
        (numItems > 0U))
    {
        HW_DMA_channelData_S * const cd = &data->channels[channel];
        cd->memory   = memory;
        cd->numItems = numItems;
        cd->status   = HW_DMA_STATUS_BUSY;
        cd->pending  = true;

        // Capture the bytes leaving memory now: the engine reads them during the
        // transfer, so a later completion would see a buffer firmware has reused.
        if (data->config->channels[channel].direction == HW_DMA_DIRECTION_MEM_TO_PERIPH)
        {
            const size_t byteLen = HW_DMA_private_byteLength(channel);
            const size_t copyLen = (byteLen < HW_DMA_SIM_MAX_BYTES) ? byteLen : HW_DMA_SIM_MAX_BYTES;
            const uint8_t * const src = (const uint8_t *)memory;
            for (size_t i = 0U; i < copyLen; i++)
            {
                cd->lastMem[i] = src[i];
            }
            cd->lastMemLen = copyLen;
        }

        // The sim twin of the transfer-complete IRQ.
        SIL_irq_pend(HW_DMA_completionIrqHandle);
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
