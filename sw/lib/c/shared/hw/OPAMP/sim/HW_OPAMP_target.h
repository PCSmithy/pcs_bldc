#pragma once

// Target-specific half of HW_OPAMP; reached via HW_OPAMP.h.

/* Includes */
#include "lib_types.h"

/* Typedefs */

// One internal operational amplifier. Carries the gain explicitly (the
// stm32g4 target derives amplification from its HAL handle) so the sim can
// model input * gain onto the internal ADC path.
typedef struct
{
    char *    channelNameStr;
    float32_t gain;
} HW_OPAMP_channelConfig_S;
