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
        .rotorEstimator = APP_ROTORESTIMATION_CHANNEL_MAIN,
        .encoder = IO_AS5048_CHANNEL_MOTOR,
        .motorPolePairs = 14U,
        .pwmPeriod_s = HW_TIM_PWM_PERIOD_S,
        .maxVoltage_V = 8.0f,      // first-spin reduced voltage; the linear limit is V_bus / sqrt(3)
        .foc =
        {
            .maxCurrent_A = 1.0f,     // under the 1.2 A phase trip
            .motorL_H = 5.0e-3f,      // GM6208-150T, as the SIL motor model has it
            .motorR_ohm = 14.7f,
            .currentLoopBandwidth_radPerSec = 1000.0f,   // ~160 Hz: w_bw T = 0.05, well under the L/R pole at 2940 rad/s
        },
        .defaultMode = APP_MOTORCONTROL_MODE_FOC,
        .prechargeTime_ms = 5U,   // bootstrap caps through the gate driver's own diodes
    },
};

const app_motorControl_config_S app_motorControl_config =
{
    .channels = app_motorControl_channelConfig,
    .numChannels = COUNTOF(app_motorControl_channelConfig),
};
