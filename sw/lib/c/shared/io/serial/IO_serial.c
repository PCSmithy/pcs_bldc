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

// [impl->fw~conn_serial_002~1]
// [impl->fw~conn_serial_003~1]
void IO_serial_write(IO_serial_channel_E channel, const uint8_t * bytes, uint32_t len)
{
    if ((data->config != NULL) && (channel < IO_SERIAL_CHANNEL_COUNT) && (bytes != NULL))
    {
        for (uint32_t i = 0U; i < len; i++)
        {
            uint32_t retries = 0U;
            while (HW_USB_write(&bytes[i], 1U) == 0U)
            {
                // Transport full: flush what is buffered, yield so the device
                // service drains it, and retry. Give up before blocking forever.
                HW_USB_writeFlush();
                HW_USB_serviceYield();
                retries++;
                if (retries >= IO_SERIAL_TX_RETRY_LIMIT)
                {
                    return;
                }
            }
        }
        HW_USB_writeFlush();
    }
}

// [impl->fw~conn_serial_004~1]
uint32_t IO_serial_available(IO_serial_channel_E channel)
{
    uint32_t count = 0U;
    if ((data->config != NULL) && (channel < IO_SERIAL_CHANNEL_COUNT))
    {
        count = HW_USB_available();
    }
    return count;
}

uint32_t IO_serial_read(IO_serial_channel_E channel, uint8_t * buffer, uint32_t len)
{
    uint32_t count = 0U;
    if ((data->config != NULL) && (channel < IO_SERIAL_CHANNEL_COUNT) && (buffer != NULL))
    {
        count = HW_USB_read(buffer, len);
    }
    return count;
}

// [impl->fw~conn_serial_005~1]
bool IO_serial_connected(IO_serial_channel_E channel)
{
    bool connected = false;
    if ((data->config != NULL) && (channel < IO_SERIAL_CHANNEL_COUNT))
    {
        connected = HW_USB_connected();
    }
    return connected;
}
