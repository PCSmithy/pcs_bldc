/* Includes */
#include "lib_utils.h"

#include "HW_TIM_channels.h"
#include "app_rotorEstimation.h"

/* Defines */

/* Private Data Definitions */

static const app_rotorEstimation_channelConfig_S app_rotorEstimation_channelConfig[] =
{
    [APP_ROTORESTIMATION_CHANNEL_MAIN] =
    {
        .encoder = IO_AS5048_CHANNEL_MOTOR,
        .naturalFrequency_radPerSec = 2.0f * PI * 50.0f,   // 50 Hz tracking bandwidth (fw~est_rotor_001)
        .dampingRatio = 1.0f,
        .correctionPeriod_s = 0.001f,   // IO_AS5048_run1ms
        .estimatePeriod_s = HW_TIM_PWM_PERIOD_S,
    },
};

const app_rotorEstimation_config_S app_rotorEstimation_config =
{
    .channels = app_rotorEstimation_channelConfig,
    .numChannels = COUNTOF(app_rotorEstimation_channelConfig),
};
