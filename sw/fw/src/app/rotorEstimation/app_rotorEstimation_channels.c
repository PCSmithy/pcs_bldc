/* Includes */
#include "lib_utils.h"

#include "app_rotorEstimation.h"

/* Defines */

/* Private Data Definitions */

static const app_rotorEstimation_channelConfig_S app_rotorEstimation_channelConfig[] =
{
    [APP_ROTORESTIMATION_CHANNEL_MAIN] =
    {
        .encoder = IO_AS5048_CHANNEL_MOTOR,
        .omega_n = 2.0f * PI * 50.0f,   // 50 Hz tracking bandwidth (fw~est_rotor_001)
        .zeta = 1.0f,
        .correctionPeriod_s = 0.001f,   // IO_AS5048_run1ms
        .estimatePeriod_s = 50.0e-6f,   // TIM1 at 20 kHz (HW_TIM_channels)
    },
};

const app_rotorEstimation_config_S app_rotorEstimation_config =
{
    .channels = app_rotorEstimation_channelConfig,
    .numChannels = COUNTOF(app_rotorEstimation_channelConfig),
};
