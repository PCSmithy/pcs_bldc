/* Includes */
#include "HW_DMA.h"
#include "stm32g4xx_hal.h"

/* Typedefs */

typedef struct
{
    DMA_HandleTypeDef hdma;   // working copy of the config handle (HAL mutates it)
    HW_DMA_completeCallback_F callback;
    void * callbackContext;
    HW_DMA_status_E status;
} HW_DMA_channelData_S;

typedef struct
{
    const HW_DMA_config_S * config;
    bool initialized;

    HW_DMA_channelData_S channels[HW_DMA_CHANNEL_COUNT];
} HW_DMA_data_S;

/* Private Function Declarations */

static void HW_DMA_private_complete(DMA_HandleTypeDef * hdma, HW_DMA_status_E status);
static void HW_DMA_private_xferCplt(DMA_HandleTypeDef * hdma);
static void HW_DMA_private_xferError(DMA_HandleTypeDef * hdma);

/* Private Data Definitions */

static HW_DMA_data_S HW_DMA_data;
static HW_DMA_data_S * const data = &HW_DMA_data;

/* Private Function Definitions */

// Resolve the HAL handle back to its logical channel and settle the transfer.
static void HW_DMA_private_complete(DMA_HandleTypeDef * hdma, HW_DMA_status_E status)
{
    for (HW_DMA_channel_E channel = 0U; channel < HW_DMA_CHANNEL_COUNT; channel++)
    {
        HW_DMA_channelData_S * const cd = &data->channels[channel];
        if (&cd->hdma == hdma)
        {
            cd->status = status;
            if (cd->callback != NULL)
            {
                cd->callback(channel, cd->callbackContext);
            }
            break;
        }
    }
}

static void HW_DMA_private_xferCplt(DMA_HandleTypeDef * hdma)
{
    HW_DMA_private_complete(hdma, HW_DMA_STATUS_COMPLETE);
}

static void HW_DMA_private_xferError(DMA_HandleTypeDef * hdma)
{
    HW_DMA_private_complete(hdma, HW_DMA_STATUS_ERROR);
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
            success &= (config->channels[channel].hdma.Instance != NULL);
        }

        if (success)
        {
            __HAL_RCC_DMAMUX1_CLK_ENABLE();
            __HAL_RCC_DMA1_CLK_ENABLE();
            __HAL_RCC_DMA2_CLK_ENABLE();

            ret = true;
            for (HW_DMA_channel_E channel = 0U; channel < HW_DMA_CHANNEL_COUNT; channel++)
            {
                HW_DMA_channelData_S * const cd = &data->channels[channel];
                cd->hdma            = config->channels[channel].hdma;   // working copy
                cd->callback        = NULL;
                cd->callbackContext = NULL;
                cd->status          = HW_DMA_STATUS_IDLE;

                ret &= (HAL_DMA_Init(&cd->hdma) == HAL_OK);
                ret &= (HAL_DMA_RegisterCallback(&cd->hdma, HAL_DMA_XFER_CPLT_CB_ID, HW_DMA_private_xferCplt) == HAL_OK);
                ret &= (HAL_DMA_RegisterCallback(&cd->hdma, HAL_DMA_XFER_ERROR_CB_ID, HW_DMA_private_xferError) == HAL_OK);
            }

            // NVIC enable + the board DMA IRQ handlers land with the SPI-DMA
            // integration (M4), where completion is bench-verified.

            data->config      = config;
            data->initialized = ret;
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
        const uint32_t periphAddress = data->config->channels[channel].periphAddress;

        uint32_t srcAddress = 0U;
        uint32_t dstAddress = 0U;
        if (cd->hdma.Init.Direction == DMA_MEMORY_TO_PERIPH)
        {
            srcAddress = (uint32_t)memory;
            dstAddress = periphAddress;
        }
        else
        {
            srcAddress = periphAddress;
            dstAddress = (uint32_t)memory;
        }

        cd->status = HW_DMA_STATUS_BUSY;
        if (HAL_DMA_Start_IT(&cd->hdma, srcAddress, dstAddress, numItems) == HAL_OK)
        {
            ret = true;
        }
        else
        {
            cd->status = HW_DMA_STATUS_ERROR;
        }
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

// [impl->fw~hal_dma_003~1]
void HW_DMA_irqHandler(HW_DMA_channel_E channel)
{
    if ((data->initialized) && (channel < HW_DMA_CHANNEL_COUNT))
    {
        HAL_DMA_IRQHandler(&data->channels[channel].hdma);
    }
}
