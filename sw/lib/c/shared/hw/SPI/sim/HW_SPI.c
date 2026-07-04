/* Includes */
#include "HW_SPI.h"
#include "HW_SPI_sim.h"

/* Defines */

// Largest transfer the loopback model captures/injects. Tests stay well
// under this; transfers longer than this still move data through the
// caller's buffers, only the captured copy is clamped.
#define HW_SPI_SIM_MAX_XFER    (256U)

// Single-bit pin mask range mirroring the stm32g4 GPIO_PIN_x encoding,
// so CS validation matches the embedded target without pulling in HAL.
#define HW_SPI_SIM_PIN_MAX     (0x8000U)

/* Typedefs */
typedef enum
{
    HW_SPI_OP_TX,
    HW_SPI_OP_RX,
    HW_SPI_OP_TXRX,
} HW_SPI_op_E;

typedef struct
{
    HW_SPI_completeCallback_F callback;
    void * callbackContext;
    HW_SPI_status_E status;

    // Parameters of the in-flight / last transfer (used to settle a
    // deferred non-blocking transfer at HW_SPI_sim_tick()).
    HW_SPI_op_E op;
    uint8_t * txData;
    uint8_t * rxData;
    size_t    length;
    bool      pending;

    // Loopback capture / injection.
    uint8_t lastTx[HW_SPI_SIM_MAX_XFER];
    size_t  lastTxLen;
    uint8_t injectedRx[HW_SPI_SIM_MAX_XFER];
    size_t  injectedRxLen;

    // Fault injection.
    bool stall;
    bool forceError;

    // Chip-select records for the last transfer.
    uint32_t        csAssertCount;
    HW_GPIO_level_E csAssertLevel;
    HW_GPIO_level_E csDeassertLevel;
} HW_SPI_channelData_S;

typedef struct
{
    const HW_SPI_config_S * config;
    bool initialized;

    HW_SPI_channelData_S channels[HW_SPI_CHANNEL_COUNT];
} HW_SPI_data_S;

/* Private Function Declarations */

static bool HW_SPI_private_channelConfigValid(const HW_SPI_config_S * const config, HW_SPI_channel_E channel);
static void HW_SPI_private_clearChannel(HW_SPI_channel_E channel);
static void HW_SPI_private_assertCs(HW_SPI_channel_E channel);
static void HW_SPI_private_deassertCs(HW_SPI_channel_E channel);
static void HW_SPI_private_captureTx(HW_SPI_channel_E channel);
static void HW_SPI_private_fillRx(HW_SPI_channel_E channel);
static bool HW_SPI_private_transfer(HW_SPI_channel_E channel, HW_SPI_op_E op, uint8_t * txData, uint8_t * rxData, size_t length);

/* Private Data Definitions */

static HW_SPI_data_S HW_SPI_data;
static HW_SPI_data_S * const data = &HW_SPI_data;

/* Private Function Definitions */

// [impl->fw~hal_spi_007~1]
static bool HW_SPI_private_channelConfigValid(const HW_SPI_config_S * const config, HW_SPI_channel_E channel)
{
    const HW_SPI_channelConfig_S * const channelConfig = &config->channels[channel];

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
                                  (csGpioConfig->pin <= HW_SPI_SIM_PIN_MAX) &&
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

static void HW_SPI_private_clearChannel(HW_SPI_channel_E channel)
{
    HW_SPI_channelData_S * const cd = &data->channels[channel];
    cd->callback        = NULL;
    cd->callbackContext = NULL;
    cd->status          = HW_SPI_STATUS_IDLE;
    cd->op              = HW_SPI_OP_TX;
    cd->txData          = NULL;
    cd->rxData          = NULL;
    cd->length          = 0U;
    cd->pending         = false;
    cd->lastTxLen       = 0U;
    cd->injectedRxLen   = 0U;
    cd->stall           = false;
    cd->forceError      = false;
    cd->csAssertCount   = 0U;
    cd->csAssertLevel   = HW_GPIO_LEVEL_LOW;
    cd->csDeassertLevel = HW_GPIO_LEVEL_LOW;
}

// [impl->fw~hal_spi_004~1]
static void HW_SPI_private_assertCs(HW_SPI_channel_E channel)
{
    const HW_SPI_channelConfig_S * const channelConfig = &data->config->channels[channel];
    if (channelConfig->csMode == HW_SPI_CS_MODE_GPIO)
    {
        const HW_SPI_csGpioConfig_S * const cs = &channelConfig->csGpioConfig;
        HW_GPIO_writePin(cs->port, cs->pin, cs->activeLevel);
        data->channels[channel].csAssertLevel = cs->activeLevel;
        data->channels[channel].csAssertCount++;
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
        data->channels[channel].csDeassertLevel = inactive;
    }
}

static void HW_SPI_private_captureTx(HW_SPI_channel_E channel)
{
    HW_SPI_channelData_S * const cd = &data->channels[channel];
    if ((cd->op == HW_SPI_OP_TX) || (cd->op == HW_SPI_OP_TXRX))
    {
        const size_t copyLen = (cd->length < HW_SPI_SIM_MAX_XFER) ? cd->length : HW_SPI_SIM_MAX_XFER;
        for (size_t i = 0U; i < copyLen; i++)
        {
            cd->lastTx[i] = cd->txData[i];
        }
        cd->lastTxLen = cd->length;
    }
}

static void HW_SPI_private_fillRx(HW_SPI_channel_E channel)
{
    HW_SPI_channelData_S * const cd = &data->channels[channel];
    if (cd->op == HW_SPI_OP_TXRX)
    {
        if (cd->injectedRxLen > 0U)
        {
            // Full-duplex with an injected response: MISO is driven from the
            // injected frame rather than mirrored from MOSI. A peripheral that
            // reads over transmitReceive (e.g. the AS5048 encoder) needs this to
            // return a crafted response; the injectedRx static is the SIL
            // white-box injection point. Falls back to loopback when nothing is
            // injected, so plain loopback consumers are unaffected.
            for (size_t i = 0U; i < cd->length; i++)
            {
                cd->rxData[i] = (i < cd->injectedRxLen) ? cd->injectedRx[i] : 0U;
            }
        }
        else
        {
            // Full-duplex loopback: MISO mirrors MOSI.
            for (size_t i = 0U; i < cd->length; i++)
            {
                cd->rxData[i] = cd->txData[i];
            }
        }
    }
    else if (cd->op == HW_SPI_OP_RX)
    {
        for (size_t i = 0U; i < cd->length; i++)
        {
            cd->rxData[i] = (i < cd->injectedRxLen) ? cd->injectedRx[i] : 0U;
        }
    }
    else
    {
        // Transmit-only: nothing to deliver to the caller.
    }
}

// [impl->fw~hal_spi_002~1]
// [impl->fw~hal_spi_003~1]
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
        HW_SPI_channelData_S * const cd = &data->channels[channel];
        const HW_SPI_bus_E bus = data->config->channels[channel].bus;
        const HW_SPI_transferMode_E mode = data->config->buses[bus].transferMode;

        cd->op     = op;
        cd->txData = txData;
        cd->rxData = rxData;
        cd->length = length;

        HW_SPI_private_assertCs(channel);
        HW_SPI_private_captureTx(channel);

        if (mode == HW_SPI_TRANSFERMODE_SW)
        {
            if (cd->stall)
            {
                HW_SPI_private_deassertCs(channel);
                cd->status = HW_SPI_STATUS_ERROR;
                ret = false;
            }
            else
            {
                HW_SPI_private_fillRx(channel);
                HW_SPI_private_deassertCs(channel);
                cd->status = HW_SPI_STATUS_COMPLETE;
                ret = true;
            }
        }
        else
        {
            // Non-blocking: leave the transfer in flight; completion (rx
            // fill, CS deassert, callback) happens at HW_SPI_sim_tick().
            cd->status  = HW_SPI_STATUS_BUSY;
            cd->pending = true;
            ret = true;
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
        for (HW_SPI_channel_E channel = 0U; channel < HW_SPI_CHANNEL_COUNT; channel++)
        {
            success &= HW_SPI_private_channelConfigValid(config, channel);
        }

        if (success)
        {
            data->config = config;
            for (HW_SPI_channel_E channel = 0U; channel < HW_SPI_CHANNEL_COUNT; channel++)
            {
                HW_SPI_private_clearChannel(channel);
            }
            data->initialized = true;
            ret = true;
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

/* SIL control + inspection (HW_SPI_sim.h) */

void HW_SPI_sim_reset(void)
{
    for (HW_SPI_channel_E channel = 0U; channel < HW_SPI_CHANNEL_COUNT; channel++)
    {
        HW_SPI_private_clearChannel(channel);
    }
}

// [impl->fw~hal_spi_005~1]
void HW_SPI_sim_tick(void)
{
    if (data->initialized)
    {
        for (HW_SPI_channel_E channel = 0U; channel < HW_SPI_CHANNEL_COUNT; channel++)
        {
            HW_SPI_channelData_S * const cd = &data->channels[channel];
            if (cd->pending)
            {
                cd->pending = false;
                if (cd->forceError)
                {
                    cd->status = HW_SPI_STATUS_ERROR;
                }
                else
                {
                    HW_SPI_private_fillRx(channel);
                    cd->status = HW_SPI_STATUS_COMPLETE;
                }
                HW_SPI_private_deassertCs(channel);

                if (cd->callback != NULL)
                {
                    cd->callback(channel, cd->callbackContext);
                }
            }
        }
    }
}

void HW_SPI_sim_setInjectedRx(HW_SPI_channel_E channel, const uint8_t * data_, size_t length)
{
    if ((channel < HW_SPI_CHANNEL_COUNT) && (data_ != NULL))
    {
        HW_SPI_channelData_S * const cd = &data->channels[channel];
        const size_t copyLen = (length < HW_SPI_SIM_MAX_XFER) ? length : HW_SPI_SIM_MAX_XFER;
        for (size_t i = 0U; i < copyLen; i++)
        {
            cd->injectedRx[i] = data_[i];
        }
        cd->injectedRxLen = copyLen;
    }
}

size_t HW_SPI_sim_getLastTx(HW_SPI_channel_E channel, uint8_t * out, size_t maxLength)
{
    size_t txLen = 0U;
    if ((channel < HW_SPI_CHANNEL_COUNT) && (out != NULL))
    {
        const HW_SPI_channelData_S * const cd = &data->channels[channel];
        txLen = cd->lastTxLen;
        const size_t captured = (cd->lastTxLen < HW_SPI_SIM_MAX_XFER) ? cd->lastTxLen : HW_SPI_SIM_MAX_XFER;
        const size_t copyLen  = (captured < maxLength) ? captured : maxLength;
        for (size_t i = 0U; i < copyLen; i++)
        {
            out[i] = cd->lastTx[i];
        }
    }
    return txLen;
}

void HW_SPI_sim_setStall(HW_SPI_channel_E channel, bool stall)
{
    if (channel < HW_SPI_CHANNEL_COUNT)
    {
        data->channels[channel].stall = stall;
    }
}

void HW_SPI_sim_setForceError(HW_SPI_channel_E channel, bool forceError)
{
    if (channel < HW_SPI_CHANNEL_COUNT)
    {
        data->channels[channel].forceError = forceError;
    }
}

uint32_t HW_SPI_sim_getCsAssertCount(HW_SPI_channel_E channel)
{
    uint32_t count = 0U;
    if (channel < HW_SPI_CHANNEL_COUNT)
    {
        count = data->channels[channel].csAssertCount;
    }
    return count;
}

HW_GPIO_level_E HW_SPI_sim_getCsAssertLevel(HW_SPI_channel_E channel)
{
    HW_GPIO_level_E level = HW_GPIO_LEVEL_LOW;
    if (channel < HW_SPI_CHANNEL_COUNT)
    {
        level = data->channels[channel].csAssertLevel;
    }
    return level;
}

HW_GPIO_level_E HW_SPI_sim_getCsDeassertLevel(HW_SPI_channel_E channel)
{
    HW_GPIO_level_E level = HW_GPIO_LEVEL_LOW;
    if (channel < HW_SPI_CHANNEL_COUNT)
    {
        level = data->channels[channel].csDeassertLevel;
    }
    return level;
}
