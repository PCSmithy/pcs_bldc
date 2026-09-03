#pragma once

/* Includes */
#include "lib_types.h"
#include "HW_OPAMP_channels.h"

/* Target Config */
#include "HW_OPAMP_target.h"   // HW_OPAMP_channelConfig_S

/* Typedefs */

typedef struct
{
    const HW_OPAMP_channelConfig_S * channels;
    uint8_t numChannels;
} HW_OPAMP_config_S;

/* Public Function Declarations */

// Initialize every amplifier listed in `config`. Validates the config
// (NULL config/channels, numChannels beyond the available amplifiers),
// then per channel: self-calibrates the input offset (internal output
// temporarily disabled) and starts the amplifier with its configured
// input/gain and its output routed to the internal ADC input. Returns
// false on any validation, calibration, or start failure.
bool HW_OPAMP_init(const HW_OPAMP_config_S * const config);
