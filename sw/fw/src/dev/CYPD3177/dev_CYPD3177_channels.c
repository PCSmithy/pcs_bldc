/* Includes */
#include "dev_CYPD3177.h"  // pulls lib_types (needed before lib_utils.h)
#include "lib_utils.h"

/* Private Data Definitions */

static const dev_CYPD3177_channelConfig_S dev_CYPD3177_channelConfig[] =
{
    [DEV_CYPD3177_CHANNEL_SINK] =
    {
        .ioDevice = IO_I2C_DEVICE_CYPD3177,
    },
};

const dev_CYPD3177_config_S dev_CYPD3177_config =
{
    .channels = dev_CYPD3177_channelConfig,
    .numChannels = COUNTOF(dev_CYPD3177_channelConfig),
};
