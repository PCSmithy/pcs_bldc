/* Includes */
#include "IO_AS5048.h"
#include "HW_SPI.h"
#include "lib_utils.h"

/* Defines */


/* Private Function Declarations */

/* Private Data Declarations */
static const IO_AS5048_channelConfig_S IO_AS5048_channelConfig[] =
{
    [IO_AS5048_CHANNEL_MOTOR] =
    {
        .spiChannel = HW_SPI_CHANNEL_AS5048_1,
        .reverse = false,
    },
    [IO_AS5048_CHANNEL_DIAL] =
    {
        .spiChannel = HW_SPI_CHANNEL_AS5048_2,
        .reverse = false,   // encoder turns opposite the LED-ring convention
    },
};

const IO_AS5048_config_S IO_AS5048_config =
{
    .channels = IO_AS5048_channelConfig,
    .numChannels = COUNTOF(IO_AS5048_channelConfig),
};

/* Private Function Definitions */


/* Public Function Definitions */

