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
            .velocityKp_NmPerRadPerSec = 0.04f,   // K_vp / J ~ 275 rad/s at J = 1.45e-4, under the estimator's 314 rad/s poles (tools/trace_analysis/velocity_loop)
            .velocityKi_NmPerRad = 0.5f,         // integral corner K_vi / K_vp = 12.5 rad/s
            .currentLoopBandwidth_radPerSec = 1000.0f,   // ~160 Hz: w_bw T = 0.05, well under the L/R pole at 2940 rad/s
            .velocityKff = 1.0f,   // full disturbance feed-forward (fw~mc_025)
            // cogging model (fw~mc_028): T_d fitted against the raw rotor angle on the FOC velocity
            // holds, observer lag advanced out (estimator_order README, 2026-11-09); the open-loop
            // sinusoidal crawl over-reads both orders with the drive's own 2 and 4/elec ripple
            .cogging = { { .order_perRev = 28U, .amplitude_Nm = 9.0e-3f, .phase_rad = 1.1f },
                         { .order_perRev = 56U, .amplitude_Nm = 0.9e-3f, .phase_rad = -1.75f } },
            .coggingKff = 1.0f,
            .positionKp_perSec = 30.0f,
            .positionKi_perSec2 = 90.0f,
            .maxPosition_rad = 20.0f * PI,   // ten turns either side of the enable point
        },
        .defaultMode = APP_MOTORCONTROL_MODE_FOC_POSITION,   // the dial moves a position; a host Command selects any method
        .prechargeTime_ms = 5U,   // bootstrap caps through the gate driver's own diodes
    },
};

const app_motorControl_config_S app_motorControl_config =
{
    .channels = app_motorControl_channelConfig,
    .numChannels = COUNTOF(app_motorControl_channelConfig),
};
