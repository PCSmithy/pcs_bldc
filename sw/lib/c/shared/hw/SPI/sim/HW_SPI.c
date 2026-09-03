/* Includes */
#include "HW_SPI.h"
#include "SIL_irq.h"
#include "SIL_ports.h"

/* Defines */

// Single-bit pin mask range mirroring the stm32g4 GPIO_PIN_x encoding,
// so CS validation matches the embedded target without pulling in HAL.
#define HW_SPI_SIM_PIN_MAX     (0x8000U)

// Completion dispatch rides the peripheral-ISR rung of the sim NVIC ladder
// (docs/sil/sim-interrupts.md), alongside sim HW_USB and HW_ADC.
#define HW_SPI_IRQ_PRIORITY    (8U)

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

    // Parameters of the in-flight / last transfer (a deferred non-blocking
    // transfer settles in the pended completion interrupt).
    HW_SPI_op_E op;
    uint8_t * txData;
    uint8_t * rxData;
    size_t    length;
    bool      pending;

    // SIL duplex endpoint (SIL_PORTS_HANDLE_INVALID when unregistered): a
    // transfer upcalls the linked peer for the MISO response frame.
    int32_t duplexHandle;

    // Fault knobs, written by DWARF from SIL: a stalled software transfer times
    // out, a forced error fails the non-blocking completion.
    bool stall;
    bool forceError;
} HW_SPI_channelData_S;

typedef struct
{
    const HW_SPI_config_S * config;
    bool initialized;

    HW_SPI_channelData_S channels[HW_SPI_CHANNEL_COUNT];
} HW_SPI_data_S;

/* Private Function Declarations */

static bool HW_SPI_private_channelConfigValid(const HW_SPI_config_S * const config, HW_SPI_channel_E channel);
static void HW_SPI_private_assertCs(HW_SPI_channel_E channel);
static void HW_SPI_private_deassertCs(HW_SPI_channel_E channel);
static void HW_SPI_private_fillRx(HW_SPI_channel_E channel);
static bool HW_SPI_private_transfer(HW_SPI_channel_E channel, HW_SPI_op_E op, uint8_t * txData, uint8_t * rxData, size_t length);
// External linkage (the HW_USB_sim_irqHandler pattern): SIL scenarios resolve
// the completion ISR by name, which -O2 strips from a static.
void HW_SPI_sim_completionDispatch(void);

/* Private Data Definitions */

static HW_SPI_data_S HW_SPI_data;
static HW_SPI_data_S * const data = &HW_SPI_data;

// The completion service's framework handle. Lives outside HW_SPI_data so the
// re-entrant init's clean slate can still cancel the previous registration.
static int32_t HW_SPI_completionIrqHandle = SIL_IRQ_HANDLE_INVALID;

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

static void HW_SPI_private_fillRx(HW_SPI_channel_E channel)
{
    HW_SPI_channelData_S * const cd = &data->channels[channel];
    if (cd->op == HW_SPI_OP_TX)
    {
        // Transmit-only: nothing to deliver to the caller.
    }
    else
    {
        // Duplex/receive: the linked peer answers with the MISO frame, handed
        // the MOSI frame on TXRX so a command-aware peer can parse it.
        const uint8_t * const tx = (cd->op == HW_SPI_OP_TXRX) ? cd->txData : NULL;
        const size_t txLen = (cd->op == HW_SPI_OP_TXRX) ? cd->length : 0U;
        size_t rxLen = 0U;
        const bool answered = SIL_ports_duplexTransfer(cd->duplexHandle, tx, txLen,
                                                       cd->rxData, cd->length, &rxLen);
        if (!answered)
        {
            for (size_t i = 0U; i < cd->length; i++)
            {
                cd->rxData[i] = 0xFFU; // floating MISO: a disconnected bus reads all-ones
            }
        }
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
    // A start on an in-flight channel is refused, as HAL_BUSY would: overwriting
    // the transfer would fold its pending completion into the new one.
    if ((data->initialized) &&
        (channel < HW_SPI_CHANNEL_COUNT) &&
        (!data->channels[channel].pending) &&
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
            // Non-blocking: leave the transfer in flight and pend the
            // completion interrupt, as the real DMA/IT hardware would.
            cd->status  = HW_SPI_STATUS_BUSY;
            cd->pending = true;
            SIL_irq_pend(HW_SPI_completionIrqHandle);
            ret = true;
        }
    }
    return ret;
}

// [impl->fw~hal_spi_005~1]
// Completion interrupt: pended at transfer time, dispatched in the firmware
// fiber's ISR bracket. The pending set is snapshotted at entry so a transfer
// started from a callback settles in the next interrupt, as hardware does.
void HW_SPI_sim_completionDispatch(void)
{
    if (data->initialized)
    {
        uint32_t due = 0U;
        for (HW_SPI_channel_E channel = 0U; channel < HW_SPI_CHANNEL_COUNT; channel++)
        {
            if (data->channels[channel].pending)
            {
                data->channels[channel].pending = false;
                due |= (1UL << channel);
            }
        }

        for (HW_SPI_channel_E channel = 0U; channel < HW_SPI_CHANNEL_COUNT; channel++)
        {
            HW_SPI_channelData_S * const cd = &data->channels[channel];
            if ((due & (1UL << channel)) != 0U)
            {
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

/* Public Function Definitions */
// [impl->fw~hal_spi_001~1]
bool HW_SPI_init(const HW_SPI_config_S * const config)
{
    bool ret = false;

    // Re-entrant: every call, accepted or rejected, is a clean slate; the
    // completion service goes with it.
    SIL_irq_cancel(HW_SPI_completionIrqHandle);
    HW_SPI_completionIrqHandle = SIL_IRQ_HANDLE_INVALID;
    *data = (HW_SPI_data_S){ 0 };
    for (HW_SPI_channel_E channel = 0U; channel < HW_SPI_CHANNEL_COUNT; channel++)
    {
        data->channels[channel].duplexHandle = SIL_PORTS_HANDLE_INVALID;
    }

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
                // One duplex endpoint per named channel, which a transfer upcalls
                // for its MISO response. Unlinked, the fill is the floating bus.
                const char * const name = config->channels[channel].channelNameStr;
                data->channels[channel].duplexHandle = (name != NULL)
                    ? SIL_ports_registerDuplex("spi", name)
                    : SIL_PORTS_HANDLE_INVALID;
            }

            // A non-blocking bus needs the completion service. With no hooks
            // installed the handle stays invalid and the transfer never settles.
            bool anyNonBlocking = false;
            for (HW_SPI_bus_E bus = 0U; bus < HW_SPI_BUS_COUNT; bus++)
            {
                anyNonBlocking = (anyNonBlocking) ||
                                 ((config->buses[bus].enabled) &&
                                  (config->buses[bus].transferMode != HW_SPI_TRANSFERMODE_SW));
            }
            if (anyNonBlocking)
            {
                HW_SPI_completionIrqHandle = SIL_irq_registerPended(
                    HW_SPI_sim_completionDispatch,
                    HW_SPI_IRQ_PRIORITY);
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
