
/* Includes */

#include "lib_types.h"

#include "HW_ADC.h"
#include "stm32g4xx_hal.h"

/* Defines */
#define HW_ADC_INPUTS_PER_ADC (16U)

/* Typedefs */
typedef struct
{
    uint32_t counts[HW_ADC_INPUTS_PER_ADC];
} HW_ADC_channelData_S;

typedef struct
{
    const HW_ADC_config_S * config;
    HW_ADC_channelData_S channelData[HW_ADC_CHANNEL_COUNT];
} HW_ADC_data_S;

/* Private Function Declarations */

/* Private Data Definitions */
static HW_ADC_data_S HW_ADC_data;
static HW_ADC_data_S * const data = &HW_ADC_data;

/* Private Function Definitions */

/* Public Function Definitions */

bool HW_ADC_init(const HW_ADC_config_S * config)
{
    bool success = true;

    for (size_t channel = 0U; channel < config->numChannels; channel++)
    {
        // Common config
        HW_ADC_channelConfig_S * channelConfig = (HW_ADC_channelConfig_S*)&config->channels[channel];
        if (HAL_ADC_Init(&channelConfig->hadc) != HAL_OK)
        {
            success = false;
            break;
        }

        // Configure the ADC multi-mode
        if (channelConfig->configureMultimode)
        {
            if (HAL_ADCEx_MultiModeConfigChannel(&channelConfig->hadc, &channelConfig->multimode) != HAL_OK)
            {
                success = false;
                break;
            }
        }

        // Configure Regular Channel
        if (HAL_ADC_ConfigChannel(&channelConfig->hadc, &channelConfig->sConfig) != HAL_OK)
        {
            success = false;
            break;
        }
    }

    if (success)
    {
        data->config = config;
    }

    return success;
}

void HW_ADC_run1ms(void)
{
    if (data->config != NULL)
    {
        // TODO - sample all configured ADC channels.

    }
}
