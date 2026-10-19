/* Includes */
#include "lib_utils.h"

#include "IO_bridge.h"
#include "IO_AS5048.h"

#include "dev_gateDriver.h"

#include "app_motorControl.h"

/* Defines */

/* Private Data Definitions */

static const app_motorControl_channelConfig_S app_motorControl_channelConfig[] =
{
    [APP_MOTORCONTROL_CHANNEL_MAIN] =
    {
        .gateDriver = DEV_GATEDRIVER_CHANNEL_MAIN,
        .bridge = IO_BRIDGE_CHANNEL_MOTOR,
        .maxVelocity_radPerSec = APP_MOTORCONTROL_MAX_VELOCITY_RAD_PER_SEC,
        .velocityEstimateFilterTau_s = 0.01f,   // 10 ms (fw~est_velocity_001 bounds)
        .encoder = IO_AS5048_CHANNEL_MOTOR,
        .motorPolePairs = 14U,
        .pwmPeriod_s = 50.0e-6f,   // TIM1 at 20 kHz (HW_TIM_channels)
        .maxVoltage_V = 8.0f,      // first-spin reduced voltage; the linear limit is V_bus / sqrt(3)
        .defaultMode = APP_MOTORCONTROL_MODE_SINUSOIDAL,
        .prechargeTime_ms = 5U,   // bootstrap caps through the gate driver's own diodes
    },
};

const app_motorControl_config_S app_motorControl_config =
{
    .channels = app_motorControl_channelConfig,
    .numChannels = COUNTOF(app_motorControl_channelConfig),
};
