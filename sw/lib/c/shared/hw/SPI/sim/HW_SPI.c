/* Includes */
#include "HW_SPI.h"

/* Defines */

/* Typedefs */
typedef struct
{
    const HW_SPI_config_S * config;
} HW_SPI_data_S;

/* Private Function Declarations */

/* Private Data Definitions */

static HW_SPI_data_S HW_SPI_data;
static HW_SPI_data_S * const data = &HW_SPI_data;

/* Private Function Definitions */

/* Public Function Definitions */
bool HW_SPI_init(const HW_SPI_config_S * const config)
{
    bool ret = false;
    if (config != NULL)
    {
        bool success = true;
        for (HW_SPI_channels_E channel = 0U; channel < HW_SPI_CHANNEL_COUNT; channel++)
        {
            if (config->channels[channel].enabled)
            {
                const HW_SPI_channelConfig_S * const channelConfig = &config->channels[channel];

                success &= channelConfig->channelNameStr != NULL;
            }
        }

        if (success)
        {
            data->config = config;
            ret = true;
        }
    }
    return ret;
}
