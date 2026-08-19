/* Includes */
#include "IO_serial.h"
#include "HW_USB.h"

/* Defines */

// Backpressure bound: how many flush+yield retries a single byte gets before it
// is dropped. Empirically large enough to ride out a host that drains slowly,
// small enough that a stalled/disconnected host cannot wedge the caller.
#define IO_SERIAL_TX_RETRY_LIMIT  1000U

/* Private Data Definitions */

typedef struct
{
    const IO_serial_config_S * config;
} IO_serial_data_S;

static IO_serial_data_S IO_serial_data;
static IO_serial_data_S * const data = &IO_serial_data;

/* Public Function Definitions */

// [impl->fw~conn_serial_001~1]
bool IO_serial_init(const IO_serial_config_S * const config)
{
    bool success = false;
    if ((config != NULL) &&
        (config->channels != NULL) &&
        (config->numChannels <= IO_SERIAL_CHANNEL_COUNT))
    {
        success = true;
        for (size_t channel = 0U; channel < config->numChannels; channel++)
        {
            if (config->channels[channel].transport >= IO_SERIAL_TRANSPORT_COUNT)
            {
                success = false;
                break;
            }
        }

        if (success)
        {
            data->config = config;
        }
    }
    return success;
}

// Each public operation dispatches on the channel's configured transport. Only
// USB CDC is wired up today; a new transport (e.g. IO_SERIAL_TRANSPORT_UART)
// adds one case to each switch below — its HW_UART-backed implementation — and
// nothing else changes.

// [impl->fw~conn_serial_002~1]
// [impl->fw~conn_serial_003~1]
void IO_serial_write(IO_serial_channel_E channel, const uint8_t * bytes, uint32_t len)
{
    if ((data->config != NULL) && (channel < IO_SERIAL_CHANNEL_COUNT) && (bytes != NULL))
    {
        switch (data->config->channels[channel].transport)
        {
            case IO_SERIAL_TRANSPORT_USB_CDC:
            {
                // Push the whole buffer in as few transport calls as possible:
                // HW_USB_write accepts up to the free FIFO space per call, so a
                // buffer that fits goes in one call (per-byte calls were the
                // dominant cost). Only stall+retry when the FIFO is actually
                // full, and give up before blocking forever.
                uint32_t sent = 0U;
                uint32_t retries = 0U;
                while (sent < len)
                {
                    const uint32_t accepted = HW_USB_write(&bytes[sent], len - sent);
                    if (accepted > 0U)
                    {
                        sent += accepted;
                        retries = 0U;
                    }
                    else
                    {
                        HW_USB_writeFlush();
                        HW_USB_serviceYield();
                        retries++;
                        if (retries >= IO_SERIAL_TX_RETRY_LIMIT)
                        {
                            break;
                        }
                    }
                }
                HW_USB_writeFlush();
                break;
            }

            // case IO_SERIAL_TRANSPORT_UART: transmit over the HW_UART channel.

            default:
                break;
        }
    }
}

// [impl->fw~conn_serial_004~1]
uint32_t IO_serial_available(IO_serial_channel_E channel)
{
    uint32_t count = 0U;
    if ((data->config != NULL) && (channel < IO_SERIAL_CHANNEL_COUNT))
    {
        switch (data->config->channels[channel].transport)
        {
            case IO_SERIAL_TRANSPORT_USB_CDC:
                count = HW_USB_available();
                break;

            // case IO_SERIAL_TRANSPORT_UART: count = HW_UART available bytes.

            default:
                break;
        }
    }
    return count;
}

uint32_t IO_serial_read(IO_serial_channel_E channel, uint8_t * buffer, uint32_t len)
{
    uint32_t count = 0U;
    if ((data->config != NULL) && (channel < IO_SERIAL_CHANNEL_COUNT) && (buffer != NULL))
    {
        switch (data->config->channels[channel].transport)
        {
            case IO_SERIAL_TRANSPORT_USB_CDC:
                count = HW_USB_read(buffer, len);
                break;

            // case IO_SERIAL_TRANSPORT_UART: read from the HW_UART channel.

            default:
                break;
        }
    }
    return count;
}

// [impl->fw~conn_serial_006~1]
uint32_t IO_serial_txFree(IO_serial_channel_E channel)
{
    uint32_t count = 0U;
    if ((data->config != NULL) && (channel < IO_SERIAL_CHANNEL_COUNT))
    {
        switch (data->config->channels[channel].transport)
        {
            case IO_SERIAL_TRANSPORT_USB_CDC:
                count = HW_USB_writeAvailable();
                break;

            // case IO_SERIAL_TRANSPORT_UART: free space in the HW_UART TX path.

            default:
                break;
        }
    }
    return count;
}

// [impl->fw~conn_serial_005~1]
bool IO_serial_isConnected(IO_serial_channel_E channel)
{
    bool connected = false;
    if ((data->config != NULL) && (channel < IO_SERIAL_CHANNEL_COUNT))
    {
        switch (data->config->channels[channel].transport)
        {
            case IO_SERIAL_TRANSPORT_USB_CDC:
                connected = HW_USB_connected();
                break;

            // case IO_SERIAL_TRANSPORT_UART: query the HW_UART connection.

            default:
                break;
        }
    }
    return connected;
}
