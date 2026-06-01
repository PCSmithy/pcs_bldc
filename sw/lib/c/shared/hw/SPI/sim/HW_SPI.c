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
        for (HW_SPI_bus_E bus = 0U; bus < HW_SPI_BUS_COUNT; bus++)
        {
            if (config->buses[bus].enabled)
            {
                const HW_SPI_busConfig_S * const busConfig = &config->buses[bus];

                success &= busConfig->busNameStr != NULL;
            }
        }

        for (HW_SPI_channel_E channel = 0U; channel < HW_SPI_CHANNEL_COUNT; channel++)
        {
            success &= (config->channels[channel].bus < HW_SPI_BUS_COUNT);
            success &= (config->channels[channel].channelNameStr != NULL);
        }

        if (success)
        {
            data->config = config;
            ret = true;
        }
    }
    return ret;
}

bool HW_SPI_transmit(HW_SPI_channel_E channel, uint8_t * txData, size_t length);
bool HW_SPI_receive(HW_SPI_channel_E channel, uint8_t * rxData, size_t length);
bool HW_SPI_transmitReceive(HW_SPI_channel_E channel, uint8_t * txData, uint8_t * rxData, size_t length);
