#ifndef HW_ADC_H
#define HW_ADC_H

/* Includes */
#include "lib_types.h"
#include "stm32g4xx_hal.h"
#include "HW_ADC_channels.h"

/* Defines */

/* Typedefs */

typedef struct
{
    ADC_HandleTypeDef hadc;

    bool configureMultimode;
    ADC_MultiModeTypeDef multimode;

    ADC_ChannelConfTypeDef sConfig;
} HW_ADC_channelConfig_S;

typedef struct
{
    const HW_ADC_channelConfig_S * channels;
    size_t numChannels;
} HW_ADC_config_S;

/* Static Inline Functions */

/* Public Function Declarations */

bool HW_ADC_init(const HW_ADC_config_S * const config);
void HW_ADC_run1ms(void);

#endif // HW_ADC_H