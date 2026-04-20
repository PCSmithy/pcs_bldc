#ifndef HW_ADC_H
#define HW_ADC_H

/* Includes */
#include "lib_types.h"
#include "HW_ADC_channels.h"

/* Defines */

/* Typedefs */

typedef struct
{
    char * channelNameStr;
} HW_ADC_channelConfig_S;

typedef struct
{
    const HW_ADC_channelConfig_S * const channels;
    size_t numChannels;
} HW_ADC_config_S;

/* Static Inline Functions */

/* Public Function Declarations */

bool HW_ADC_init(const HW_ADC_config_S * config);
void HW_ADC_run1ms(void);

#endif // HW_ADC_H