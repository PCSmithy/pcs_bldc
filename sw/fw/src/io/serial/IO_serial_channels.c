/* Includes */
#include "IO_serial.h"
#include "lib_utils.h"   // COUNTOF

/* Private Data Definitions */

static const IO_serial_channelConfig_S IO_serial_channelConfig[] =
{
    [IO_SERIAL_CHANNEL_CDC] =
    {
        .transport = IO_SERIAL_TRANSPORT_USB_CDC,
    },
};

const IO_serial_config_S IO_serial_config =
{
    .channels    = IO_serial_channelConfig,
    .numChannels = COUNTOF(IO_serial_channelConfig),
};
