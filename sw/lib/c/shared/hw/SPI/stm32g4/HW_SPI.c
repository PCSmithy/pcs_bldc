/* Includes */
#include "HW_SPI.h"
#include "stm32g4xx_hal.h"

/* Defines */

/* Typedefs */
typedef struct
{
    SPI_HandleTypeDef hspi;
} HW_SPI_busData_S;

typedef struct
{
    const HW_SPI_config_S * config;

    HW_SPI_busData_S buses[HW_SPI_BUS_COUNT];
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

            for (HW_SPI_bus_E bus = 0U; bus < HW_SPI_BUS_COUNT; bus++)
            {
                if (config->buses[bus].enabled)
                {
                    data->buses[bus].hspi = config->buses[bus].hspi;

                    ret &= HAL_SPI_Init(&data->buses[bus].hspi) == HAL_OK;
                }
            }

            // TODO - add cs config validation

            data->config = config;
        }
    }
    return ret;
}


bool HW_SPI_transmit(HW_SPI_channel_E channel, uint8_t * txData, size_t length);
bool HW_SPI_receive(HW_SPI_channel_E channel, uint8_t * rxData, size_t length);
bool HW_SPI_transmitReceive(HW_SPI_channel_E channel, uint8_t * txData, uint8_t * rxData, size_t length);
