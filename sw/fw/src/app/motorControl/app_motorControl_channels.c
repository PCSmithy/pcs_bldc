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
        .maxVoltage_V = 11.5f,     // the 20 V bus's linear limit, V_bus / sqrt(3); the bus term governs on a sagged bus
        .foc =
        {
            .maxCurrent_A = 1.0f,     // under the 1.2 A phase trip
            .motorL_H = 5.0e-3f,      // GM6208-150T, as the SIL motor model has it
            .motorR_ohm = 14.7f,
            .torqueConstant_NmPerA = 0.825f,   // 1.5 * K_e, K_e = 0.55 V/(rad/s)
            .velocityKp_NmPerRadPerSec = 0.04f,   // J * w_v: J = 3.0e-4 kg m^2, w_v = 50 rad/s = 0.015f
            .velocityKi_NmPerRad = 0.5f,         // K_vp * w_v / 10 = 0.075
            .currentLoopBandwidth_radPerSec = 1000.0f,   // ~160 Hz: w_bw T = 0.05, well under the L/R pole at 2940 rad/s
        },
        .defaultMode = APP_MOTORCONTROL_MODE_FOC_VELOCITY,
        .prechargeTime_ms = 5U,   // bootstrap caps through the gate driver's own diodes
    },
};

const app_motorControl_config_S app_motorControl_config =
{
    .channels = app_motorControl_channelConfig,
    .numChannels = COUNTOF(app_motorControl_channelConfig),
};
