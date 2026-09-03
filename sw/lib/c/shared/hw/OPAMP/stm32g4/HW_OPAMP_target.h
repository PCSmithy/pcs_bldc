#pragma once

// Target-specific half of HW_OPAMP; reached via HW_OPAMP.h.

/* Includes */
#include "lib_types.h"
#include "stm32g4xx_hal.h"

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
