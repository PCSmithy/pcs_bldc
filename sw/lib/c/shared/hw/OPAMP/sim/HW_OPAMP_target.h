#pragma once

// Target-specific half of HW_OPAMP; reached via HW_OPAMP.h.

/* Includes */
#include "lib_types.h"
#include "HW_OPAMP_channels.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Typedefs */

// One internal operational amplifier. Carries the gain explicitly (the
// stm32g4 target derives amplification from its HAL handle) so the sim can
// model input * gain onto the internal ADC path.
typedef struct
{
    char *    channelNameStr;
    float32_t gain;
} HW_OPAMP_channelConfig_S;

/* Public Function Declarations */

// The volts an initialized amplifier drives onto its internal ADC input at this
// instant: its input pin volts x configured gain. The sim ADC's conversion of
// that input reads it. False before init or for a channel init did not start.
bool HW_OPAMP_getOutputVolts(HW_OPAMP_channels_E channel, float32_t * const out);

#ifdef __cplusplus
}
#endif
