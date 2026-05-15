/* Includes */
#include "HW_SPI.h"
#include "stm32g4xx_hal.h"

/* Defines */

/* Typedefs */
typedef struct
{
    SPI_HandleTypeDef hspi;
} HW_SPI_channelData_S;

typedef struct
{
    const HW_SPI_config_S * config;

    HW_SPI_channelData_S channels[HW_SPI_CHANNEL_COUNT];
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

        if (success)
        {
            ret = true;
            data->config = config;

            for (HW_SPI_channels_E channel = 0U; channel < HW_SPI_CHANNEL_COUNT; channel++)
            {
                if (config->channels[channel].enabled)
                {
                    data->channels[channel].hspi = config->channels[channel].hspi;

                    ret &= HAL_SPI_Init(&data->channels[channel].hspi) == HAL_OK;
                }
            }
        }
    }
    return ret;
}