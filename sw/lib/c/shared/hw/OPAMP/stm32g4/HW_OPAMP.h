#ifndef HW_OPAMP_H
#define HW_OPAMP_H

/* Includes */
#include "lib_types.h"
#include "stm32g4xx_hal.h"
#include "HW_OPAMP_channels.h"

/* Typedefs */

// One internal operational amplifier.
//
// Library-managed hopamp.Init fields (whatever you put here is
// overwritten by HW_OPAMP_init during its calibrate-then-run sequence):
//   - InternalOutput   <- forced DISABLE for self-calibration (the HAL
//                          rejects HAL_OPAMP_SelfCalibrate while the
//                          internal ADC output is enabled), then ENABLE so
//                          the amplifier drives its internal ADC input.
//   - UserTrimming     <- set to OPAMP_TRIMMING_USER by self-calibration.
//   - TrimmingValueN/P <- offset trims written by self-calibration.
//
// All other Init fields (Mode, NonInvertingInput, PgaGain, PgaConnect,
// PowerMode, TimerControlledMuxmode) are taken as-is from this config.
typedef struct
{
    OPAMP_HandleTypeDef hopamp;
} HW_OPAMP_channelConfig_S;

typedef struct
{
    const HW_OPAMP_channelConfig_S * channels;
    uint8_t numChannels;
} HW_OPAMP_config_S;

/* Public Function Declarations */

// Initialize every amplifier listed in `config`. Validates the config
// (NULL config/channels, numChannels beyond the available amplifiers),
// copies each user hopamp handle into internal mutable storage, then per
// channel: self-calibrates the input offset (internal output temporarily
// disabled) and starts the amplifier with its configured input/gain and
// its output routed to the internal ADC input. Returns false on any
// validation, calibration, or start failure.
bool HW_OPAMP_init(const HW_OPAMP_config_S * const config);

#endif // HW_OPAMP_H
