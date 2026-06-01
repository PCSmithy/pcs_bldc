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

static bool HW_SPI_private_channelConfigValid(const HW_SPI_config_S * const config, HW_SPI_channel_E channel);

/* Private Data Definitions */

static HW_SPI_data_S HW_SPI_data;
static HW_SPI_data_S * const data = &HW_SPI_data;

/* Private Function Definitions */

// [impl->fw~hal_spi_007~1]
static bool HW_SPI_private_channelConfigValid(const HW_SPI_config_S * const config, HW_SPI_channel_E channel)
{
    const HW_SPI_channelConfig_S * const channelConfig = &config->channels[channel];

    // Short-circuit guards the bus[] index against an out-of-range bus.
    const bool busValid = (channelConfig->bus < HW_SPI_BUS_COUNT) &&
                          (config->buses[channelConfig->bus].enabled);

    bool csValid = false;
    switch (channelConfig->csMode)
    {
        case HW_SPI_CS_MODE_NONE:
        case HW_SPI_CS_MODE_HW:
            csValid = true;
            break;

        case HW_SPI_CS_MODE_GPIO:
        {
            const HW_SPI_csGpioConfig_S * const csGpioConfig = &channelConfig->csGpioConfig;
            const bool portValid = (csGpioConfig->port < HW_GPIO_PORT_COUNT);
            // Exactly one pin selected, within the 16-pin GPIO range.
            const bool pinValid = (csGpioConfig->pin != 0U) &&
                                  (csGpioConfig->pin <= GPIO_PIN_15) &&
                                  ((csGpioConfig->pin & (csGpioConfig->pin - 1U)) == 0U);
            csValid = (portValid && pinValid);
            break;
        }

        default:
            csValid = false;
            break;
    }

    return (busValid && csValid);
}

/* Public Function Definitions */
// [impl->fw~hal_spi_001~1]
bool HW_SPI_init(const HW_SPI_config_S * const config)
{
    bool ret = false;
    if (config != NULL)
    {
        bool success = true;

        // Validate every channel's bus mapping and chip-select config
        // before touching any hardware.
        for (HW_SPI_channel_E channel = 0U; channel < HW_SPI_CHANNEL_COUNT; channel++)
        {
            success &= HW_SPI_private_channelConfigValid(config, channel);
        }

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

            data->config = config;
        }
    }
    return ret;
}


bool HW_SPI_transmit(HW_SPI_channel_E channel, uint8_t * txData, size_t length);
bool HW_SPI_receive(HW_SPI_channel_E channel, uint8_t * rxData, size_t length);
bool HW_SPI_transmitReceive(HW_SPI_channel_E channel, uint8_t * txData, uint8_t * rxData, size_t length);
