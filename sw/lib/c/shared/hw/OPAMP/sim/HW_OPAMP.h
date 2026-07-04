#ifndef HW_OPAMP_H
#define HW_OPAMP_H

/* Includes */
#include "lib_types.h"
#include "HW_OPAMP_channels.h"

/* Typedefs */

// Mirror of the stm32g4 struct. Lacks the HAL handle; carries only the
// gain (the stm32g4 impl derives amplification from the handle's
// Init.PgaGain) so the sim can model input * gain onto the internal ADC
// path. channelNameStr aids sim trace logs.
typedef struct
{
    char *    channelNameStr;
    float32_t gain;
} HW_OPAMP_channelConfig_S;

typedef struct
{
    const HW_OPAMP_channelConfig_S * channels;
    uint8_t numChannels;
} HW_OPAMP_config_S;

/* Public Function Declarations */

bool HW_OPAMP_init(const HW_OPAMP_config_S * const config);

#endif // HW_OPAMP_H
