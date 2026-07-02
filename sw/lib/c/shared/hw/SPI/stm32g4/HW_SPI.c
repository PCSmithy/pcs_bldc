/* Includes */
#include "HW_SPI.h"
#include "HW_SPI_timeout.h"
#include "stm32g4xx_hal.h"

/* Defines */

/* Typedefs */
typedef enum
{
    HW_SPI_OP_TX,
    HW_SPI_OP_RX,
    HW_SPI_OP_TXRX,
} HW_SPI_op_E;

typedef struct
{
    SPI_HandleTypeDef hspi;
    HW_SPI_channel_E  activeChannel;   // channel whose DMA transfer is in flight
} HW_SPI_busData_S;

typedef struct
{
    HW_SPI_completeCallback_F callback;
    void * callbackContext;
    HW_SPI_status_E status;
} HW_SPI_channelData_S;

typedef struct
{
    const HW_SPI_config_S * config;
    bool initialized;

    HW_SPI_busData_S buses[HW_SPI_BUS_COUNT];
    HW_SPI_channelData_S channels[HW_SPI_CHANNEL_COUNT];
} HW_SPI_data_S;

/* Private Function Declarations */

static bool     HW_SPI_private_channelConfigValid(const HW_SPI_config_S * const config, HW_SPI_channel_E channel);
static uint32_t HW_SPI_private_prescalerToDivisor(uint32_t prescaler);
static uint32_t HW_SPI_private_busBitRate(const SPI_HandleTypeDef * const hspi);
static void     HW_SPI_private_assertCs(HW_SPI_channel_E channel);
static void     HW_SPI_private_deassertCs(HW_SPI_channel_E channel);
static void     HW_SPI_private_dmaTxComplete(HW_DMA_channel_E dmaChannel, void * context);
static bool     HW_SPI_private_transfer(HW_SPI_channel_E channel, HW_SPI_op_E op, uint8_t * txData, uint8_t * rxData, size_t length);

/* Private Data Definitions */

static HW_SPI_data_S HW_SPI_data;
static HW_SPI_data_S * const data = &HW_SPI_data;

/* Private Function Definitions */

// [impl->fw~hal_spi_007~1]
static bool HW_SPI_private_channelConfigValid(const HW_SPI_config_S * const config, HW_SPI_channel_E channel)
{
    const HW_SPI_channelConfig_S * const channelConfig = &config->channels[channel];

    // Short-circuit guards the bus[] index against an out-of-range bus.
    const bool busValid = (channelConfig->bus < HW_SPI_BUS_COUNT) &&
                          (config->buses[channelConfig->bus].enabled);

    bool csValid = false;
    switch (channelConfig->csMode)
    {
        case HW_SPI_CS_MODE_NONE:
        case HW_SPI_CS_MODE_HW:
            csValid = true;
            break;

        case HW_SPI_CS_MODE_GPIO:
        {
            const HW_SPI_csGpioConfig_S * const csGpioConfig = &channelConfig->csGpioConfig;
            const bool portValid = (csGpioConfig->port < HW_GPIO_PORT_COUNT);
            // Exactly one pin selected, within the 16-pin GPIO range.
            const bool pinValid = (csGpioConfig->pin != 0U) &&
                                  (csGpioConfig->pin <= GPIO_PIN_15) &&
                                  ((csGpioConfig->pin & (csGpioConfig->pin - 1U)) == 0U);
            csValid = (portValid && pinValid);
            break;
        }

        default:
            csValid = false;
            break;
    }

    return (busValid && csValid);
}

static uint32_t HW_SPI_private_prescalerToDivisor(uint32_t prescaler)
{
    uint32_t divisor = 2U;
    switch (prescaler)
    {
        case SPI_BAUDRATEPRESCALER_2:   divisor = 2U;   break;
        case SPI_BAUDRATEPRESCALER_4:   divisor = 4U;   break;
        case SPI_BAUDRATEPRESCALER_8:   divisor = 8U;   break;
        case SPI_BAUDRATEPRESCALER_16:  divisor = 16U;  break;
        case SPI_BAUDRATEPRESCALER_32:  divisor = 32U;  break;
        case SPI_BAUDRATEPRESCALER_64:  divisor = 64U;  break;
        case SPI_BAUDRATEPRESCALER_128: divisor = 128U; break;
        case SPI_BAUDRATEPRESCALER_256: divisor = 256U; break;
        default:                        divisor = 2U;   break;
    }
    return divisor;
}

static uint32_t HW_SPI_private_busBitRate(const SPI_HandleTypeDef * const hspi)
{
    // SPI1 is clocked from APB2; SPI2/SPI3 from APB1 on the STM32G4.
    const uint32_t pclk = (hspi->Instance == SPI1) ? HAL_RCC_GetPCLK2Freq() : HAL_RCC_GetPCLK1Freq();
    return pclk / HW_SPI_private_prescalerToDivisor(hspi->Init.BaudRatePrescaler);
}

// [impl->fw~hal_spi_004~1]
static void HW_SPI_private_assertCs(HW_SPI_channel_E channel)
{
    const HW_SPI_channelConfig_S * const channelConfig = &data->config->channels[channel];
    if (channelConfig->csMode == HW_SPI_CS_MODE_GPIO)
    {
        const HW_SPI_csGpioConfig_S * const cs = &channelConfig->csGpioConfig;
        HW_GPIO_writePin(cs->port, cs->pin, cs->activeLevel);
    }
}

// [impl->fw~hal_spi_004~1]
static void HW_SPI_private_deassertCs(HW_SPI_channel_E channel)
{
    const HW_SPI_channelConfig_S * const channelConfig = &data->config->channels[channel];
    if (channelConfig->csMode == HW_SPI_CS_MODE_GPIO)
    {
        const HW_SPI_csGpioConfig_S * const cs = &channelConfig->csGpioConfig;
        const HW_GPIO_level_E inactive = (cs->activeLevel == HW_GPIO_LEVEL_LOW) ? HW_GPIO_LEVEL_HIGH : HW_GPIO_LEVEL_LOW;
        HW_GPIO_writePin(cs->port, cs->pin, inactive);
    }
}

// [impl->fw~hal_spi_005~1]
// DMA transfer-complete for a bus's TX stream (registered with HW_DMA at init).
// The DMA has moved every byte into the SPI TX FIFO; per RM0440 we wait for the
// FIFO to drain and the bus to go idle, tear down the DMA request, then settle
// the channel and fire its completion callback. The FIFO/BSY spins are bounded
// (a few byte-times) and run in the DMA ISR — kept deliberately short.
static void HW_SPI_private_dmaTxComplete(HW_DMA_channel_E dmaChannel, void * context)
{
    const HW_SPI_bus_E bus = (HW_SPI_bus_E)(uintptr_t)context;
    SPI_HandleTypeDef * const hspi = &data->buses[bus].hspi;

    while ((hspi->Instance->SR & SPI_SR_FTLVL) != 0U) { }
    while ((hspi->Instance->SR & SPI_SR_BSY) != 0U)   { }

    CLEAR_BIT(hspi->Instance->CR2, SPI_CR2_TXDMAEN);
    __HAL_SPI_DISABLE(hspi);

    const HW_SPI_channel_E channel = data->buses[bus].activeChannel;
    HW_SPI_private_deassertCs(channel);

    const bool ok = (HW_DMA_getStatus(dmaChannel) == HW_DMA_STATUS_COMPLETE);
    data->channels[channel].status = (ok) ? HW_SPI_STATUS_COMPLETE : HW_SPI_STATUS_ERROR;

    if (data->channels[channel].callback != NULL)
    {
        data->channels[channel].callback(channel, data->channels[channel].callbackContext);
    }
}

// [impl->fw~hal_spi_002~1]
// [impl->fw~hal_spi_003~1]
// [impl->fw~hal_spi_005~1]
// [impl->fw~hal_spi_006~1]
static bool HW_SPI_private_transfer(HW_SPI_channel_E channel, HW_SPI_op_E op, uint8_t * txData, uint8_t * rxData, size_t length)
{
    bool argsValid = false;
    switch (op)
    {
        case HW_SPI_OP_TX:   argsValid = (txData != NULL);                       break;
        case HW_SPI_OP_RX:   argsValid = (rxData != NULL);                       break;
        case HW_SPI_OP_TXRX: argsValid = ((txData != NULL) && (rxData != NULL)); break;
        default:             argsValid = false;                                  break;
    }

    bool ret = false;
    if ((data->initialized) &&
        (channel < HW_SPI_CHANNEL_COUNT) &&
        (length > 0U) &&
        (argsValid))
    {
        const HW_SPI_bus_E bus = data->config->channels[channel].bus;
        const HW_SPI_busConfig_S * const busConfig = &data->config->buses[bus];
        SPI_HandleTypeDef * const hspi = &data->buses[bus].hspi;

        if (busConfig->transferMode == HW_SPI_TRANSFERMODE_SW)
        {
            const uint32_t fBit      = HW_SPI_private_busBitRate(hspi);
            const uint32_t timeoutMs = HW_SPI_computeTimeoutMs(fBit, length);

            data->channels[channel].status = HW_SPI_STATUS_BUSY;
            HW_SPI_private_assertCs(channel);

            HAL_StatusTypeDef halStatus = HAL_ERROR;
            switch (op)
            {
                case HW_SPI_OP_TX:   halStatus = HAL_SPI_Transmit(hspi, txData, (uint16_t)length, timeoutMs);                break;
                case HW_SPI_OP_RX:   halStatus = HAL_SPI_Receive(hspi, rxData, (uint16_t)length, timeoutMs);                 break;
                case HW_SPI_OP_TXRX: halStatus = HAL_SPI_TransmitReceive(hspi, txData, rxData, (uint16_t)length, timeoutMs); break;
                default:             halStatus = HAL_ERROR;                                                                  break;
            }

            HW_SPI_private_deassertCs(channel);

            ret = (halStatus == HAL_OK);
            data->channels[channel].status = (ret) ? HW_SPI_STATUS_COMPLETE : HW_SPI_STATUS_ERROR;
        }
        else if (busConfig->transferMode == HW_SPI_TRANSFERMODE_DMA)
        {
            // DMA-backed transmit: kick the TX DMA stream and return; completion
            // (FIFO drain, CS deassert, status, callback) runs in
            // HW_SPI_private_dmaTxComplete off the DMA IRQ. TX-only for now;
            // RX / full-duplex over DMA (the encoder path) is pending.
            if (op == HW_SPI_OP_TX)
            {
                data->channels[channel].status = HW_SPI_STATUS_BUSY;
                data->buses[bus].activeChannel = channel;

                HW_SPI_private_assertCs(channel);
                __HAL_SPI_ENABLE(hspi);
                SET_BIT(hspi->Instance->CR2, SPI_CR2_TXDMAEN);

                ret = HW_DMA_startTransfer(busConfig->txDmaChannel, txData, (uint32_t)length);
                if (!ret)
                {
                    CLEAR_BIT(hspi->Instance->CR2, SPI_CR2_TXDMAEN);
                    __HAL_SPI_DISABLE(hspi);
                    HW_SPI_private_deassertCs(channel);
                    data->channels[channel].status = HW_SPI_STATUS_ERROR;
                }
            }
            else
            {
                ret = false;
            }
        }
        else
        {
            // Interrupt-mode transfers are not implemented.
            ret = false;
        }
    }
    return ret;
}

/* Public Function Definitions */
// [impl->fw~hal_spi_001~1]
bool HW_SPI_init(const HW_SPI_config_S * const config)
{
    bool ret = false;
    if (config != NULL)
    {
        bool success = true;

        // Validate every channel's bus mapping and chip-select config
        // before touching any hardware.
        for (HW_SPI_channel_E channel = 0U; channel < HW_SPI_CHANNEL_COUNT; channel++)
        {
            success &= HW_SPI_private_channelConfigValid(config, channel);
        }

        if (success)
        {
            ret = true;

            for (HW_SPI_bus_E bus = 0U; bus < HW_SPI_BUS_COUNT; bus++)
            {
                if (config->buses[bus].enabled)
                {
                    data->buses[bus].hspi = config->buses[bus].hspi;
                    data->buses[bus].activeChannel = (HW_SPI_channel_E)0;

                    ret &= HAL_SPI_Init(&data->buses[bus].hspi) == HAL_OK;

                    // DMA-backed bus: route its TX DMA channel's completion back
                    // into this bus. HW_DMA must be initialised before HW_SPI.
                    if (config->buses[bus].transferMode == HW_SPI_TRANSFERMODE_DMA)
                    {
                        ret &= HW_DMA_registerCallback(config->buses[bus].txDmaChannel,
                                                       HW_SPI_private_dmaTxComplete,
                                                       (void *)(uintptr_t)bus);
                    }
                }
            }

            for (HW_SPI_channel_E channel = 0U; channel < HW_SPI_CHANNEL_COUNT; channel++)
            {
                data->channels[channel].callback        = NULL;
                data->channels[channel].callbackContext = NULL;
                data->channels[channel].status          = HW_SPI_STATUS_IDLE;
            }

            data->config      = config;
            data->initialized = ret;
        }
    }
    return ret;
}

// [impl->fw~hal_spi_002~1]
bool HW_SPI_transmit(HW_SPI_channel_E channel, uint8_t * txData, size_t length)
{
    return HW_SPI_private_transfer(channel, HW_SPI_OP_TX, txData, NULL, length);
}

// [impl->fw~hal_spi_002~1]
bool HW_SPI_receive(HW_SPI_channel_E channel, uint8_t * rxData, size_t length)
{
    return HW_SPI_private_transfer(channel, HW_SPI_OP_RX, NULL, rxData, length);
}

// [impl->fw~hal_spi_002~1]
bool HW_SPI_transmitReceive(HW_SPI_channel_E channel, uint8_t * txData, uint8_t * rxData, size_t length)
{
    return HW_SPI_private_transfer(channel, HW_SPI_OP_TXRX, txData, rxData, length);
}

// [impl->fw~hal_spi_005~1]
bool HW_SPI_registerCallback(HW_SPI_channel_E channel, HW_SPI_completeCallback_F callback, void * context)
{
    bool ret = false;
    if ((data->initialized) && (channel < HW_SPI_CHANNEL_COUNT))
    {
        data->channels[channel].callback        = callback;
        data->channels[channel].callbackContext = context;
        ret = true;
    }
    return ret;
}

// [impl->fw~hal_spi_005~1]
HW_SPI_status_E HW_SPI_getStatus(HW_SPI_channel_E channel)
{
    HW_SPI_status_E status = HW_SPI_STATUS_ERROR;
    if ((data->initialized) && (channel < HW_SPI_CHANNEL_COUNT))
    {
        status = data->channels[channel].status;
    }
    return status;
}
