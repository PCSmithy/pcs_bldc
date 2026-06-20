/* Includes */
#include "IO_SK6805.h"
#include "HW_SPI.h"
#include "lib_utils.h"   // COUNTOF

/* Private Data Definitions */

static const IO_SK6805_channelConfig_S IO_SK6805_channelConfig[] =
{
    [IO_SK6805_CHANNEL_RING] =
    {
        .spiChannel = HW_SPI_CHANNEL_SK6805_STRING,
        .invert     = true,  // SK6805 data line is driven via an inverting level shifter
    },
};

const IO_SK6805_config_S IO_SK6805_config =
{
    .channels    = IO_SK6805_channelConfig,
    .numChannels = COUNTOF(IO_SK6805_channelConfig),
};
