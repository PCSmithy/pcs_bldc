/* Includes */

#include "lib_types.h"
#include "lib_utils.h"
#include "lib_build.h"

#include "HW_OPAMP.h"
// HAL types in the BUILD_TARGET_STM32G4 branch (OPAMP_HandleTypeDef, OPAMP1,
// OPAMP_PGA_MODE, etc.) come transitively through HW_OPAMP.h, which on the
// embedded target resolves to stm32g4/HW_OPAMP.h (which #includes
// stm32g4xx_hal.h). On the native target the elided branch is never parsed
// so no HAL include is needed.

/* Defines */

/* Typedefs */

/* Private Data Definitions */

// Board wiring: OPAMP1 in = PA3 (PHASE_U_VSENSE, IO1), OPAMP2 in = PB0
// (PHASE_V_VSENSE, IO2), OPAMP3 in = PA1 (PHASE_W_VSENSE, IO2). All run
// PGA x2, normal speed, output routed to the internal ADC input. The GPIO
// analog config lives in HAL_OPAMP_MspInit. InternalOutput / UserTrimming /
// TrimmingValue* are library-managed during calibration.
const HW_OPAMP_channelConfig_S HW_OPAMP_channelConfig[HW_OPAMP_CHANNEL_COUNT] =
{
#if (BUILD_TARGET == BUILD_TARGET_STM32G4)
    [HW_OPAMP_CHANNEL_1] =
    {
        .hopamp =
        {
            .Instance = OPAMP1,
            .Init =
            {
                .PowerMode              = OPAMP_POWERMODE_NORMALSPEED,
                .Mode                   = OPAMP_PGA_MODE,
                .NonInvertingInput      = OPAMP_NONINVERTINGINPUT_IO1,
                .InternalOutput         = ENABLE,
                .TimerControlledMuxmode = OPAMP_TIMERCONTROLLEDMUXMODE_DISABLE,
                .PgaConnect             = OPAMP_PGA_CONNECT_INVERTINGINPUT_NO,
                .PgaGain                = OPAMP_PGA_GAIN_2_OR_MINUS_1,
                .UserTrimming           = OPAMP_TRIMMING_FACTORY,
            },
        },
    },

    [HW_OPAMP_CHANNEL_2] =
    {
        .hopamp =
        {
            .Instance = OPAMP2,
            .Init =
            {
                .PowerMode              = OPAMP_POWERMODE_NORMALSPEED,
                .Mode                   = OPAMP_PGA_MODE,
                .NonInvertingInput      = OPAMP_NONINVERTINGINPUT_IO2,
                .InternalOutput         = ENABLE,
                .TimerControlledMuxmode = OPAMP_TIMERCONTROLLEDMUXMODE_DISABLE,
                .PgaConnect             = OPAMP_PGA_CONNECT_INVERTINGINPUT_NO,
                .PgaGain                = OPAMP_PGA_GAIN_2_OR_MINUS_1,
                .UserTrimming           = OPAMP_TRIMMING_FACTORY,
            },
        },
    },

    [HW_OPAMP_CHANNEL_3] =
    {
        .hopamp =
        {
            .Instance = OPAMP3,
            .Init =
            {
                .PowerMode              = OPAMP_POWERMODE_NORMALSPEED,
                .Mode                   = OPAMP_PGA_MODE,
                .NonInvertingInput      = OPAMP_NONINVERTINGINPUT_IO2,
                .InternalOutput         = ENABLE,
                .TimerControlledMuxmode = OPAMP_TIMERCONTROLLEDMUXMODE_DISABLE,
                .PgaConnect             = OPAMP_PGA_CONNECT_INVERTINGINPUT_NO,
                .PgaGain                = OPAMP_PGA_GAIN_2_OR_MINUS_1,
                .UserTrimming           = OPAMP_TRIMMING_FACTORY,
            },
        },
    },

#elif (BUILD_TARGET == BUILD_TARGET_SIM)
    [HW_OPAMP_CHANNEL_1] = { .channelNameStr = "OPAMP1", .gain = 2.0f },
    [HW_OPAMP_CHANNEL_2] = { .channelNameStr = "OPAMP2", .gain = 2.0f },
    [HW_OPAMP_CHANNEL_3] = { .channelNameStr = "OPAMP3", .gain = 2.0f },
#else
#error "ERROR! HW_OPAMP_channelConfig not defined for build target!"
#endif
};

const HW_OPAMP_config_S HW_OPAMP_config =
{
    .channels    = HW_OPAMP_channelConfig,
    .numChannels = COUNTOF(HW_OPAMP_channelConfig),
};

/* Private Function Definitions */

/* Public Function Definitions */
