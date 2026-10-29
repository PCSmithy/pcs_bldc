#pragma once

/* Includes */
#include "lib_types.h"

#include "IO_bridge.h"
#include "IO_AS5048.h"

#include "dev_gateDriver.h"

#include "app_rotorEstimation.h"
#include "app_motorControl_channels.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Defines */


/* Typedefs */

typedef enum
{
    APP_MOTORCONTROL_MODE_OFF,
    APP_MOTORCONTROL_MODE_SIX_STEP_TRAP,
    APP_MOTORCONTROL_MODE_SINUSOIDAL,
    APP_MOTORCONTROL_MODE_FOC,
    APP_MOTORCONTROL_MODE_COUNT,
} app_motorControl_mode_E; // TODO - convert this to `enum class` if/when consumers are C++

typedef enum
{
    APP_MOTORCONTROL_AXIS_D,
    APP_MOTORCONTROL_AXIS_Q,
    APP_MOTORCONTROL_AXIS_COUNT,
} app_motorControl_axis_E;

// Coarse bridge state for the operator display (fw~mc_009): DISABLED (bridge
// dark), ENABLED (bridge driving), or FAULTED (a latched fault holds it off).
typedef enum
{
    APP_MOTORCONTROL_STATE_DISABLED,
    APP_MOTORCONTROL_STATE_ENABLED,
    APP_MOTORCONTROL_STATE_FAULTED,
    APP_MOTORCONTROL_STATE_COUNT,
} app_motorControl_state_E;

typedef struct
{
    float32_t maxCurrent_A;  // I_max: the FOC Q-axis current a full-scale target commands (fw~mc_022)
    float32_t motorL_H;
    float32_t motorR_ohm;
    float32_t currentLoopBandwidth_radPerSec;
} app_motorControl_focConfig_S;

typedef struct
{
    dev_gateDriver_channel_E gateDriver;
    IO_bridge_channel_E bridge;

    float32_t maxVelocity_radPerSec;

    app_rotorEstimation_channel_E rotorEstimator;   // the rotor angle + velocity estimate (fw~est_rotor_002)
    IO_AS5048_channel_E encoder;   // its encoder, read here only for the fault status (fw~safety_002)

    uint8_t motorPolePairs;
    float32_t pwmPeriod_s;   // the bridge's PWM period: the step's time base
    float32_t maxVoltage_V;  // V_max: the voltage a full-scale target commands (fw~mc_020)

    app_motorControl_focConfig_S foc;

    app_motorControl_mode_E defaultMode;   // the method selected at power-up; never OFF
    uint32_t prechargeTime_ms;   // low sides on before any enable drives: the bootstrap charge (fw~mc_021)
} app_motorControl_channelConfig_S;

typedef struct
{
    const app_motorControl_channelConfig_S * channels;
    size_t numChannels;
} app_motorControl_config_S;

// Telemetry/diagnostic view of one channel's live state, and the source the
// operator display (app_rgbLedRing) reads for state + speedometer indication.
typedef struct
{
    app_motorControl_mode_E  mode;
    app_motorControl_state_E state;                        // disabled / enabled / faulted
    bool                     isAligned;
    float32_t                magneticAngle_rad;            // rotor electrical angle
    float32_t                velocitySetpoint_radPerSec;   // signed commanded speed target
    float32_t                velocityMeasured_radPerSec;   // the rotor estimator's velocity (fw~est_rotor_002)
    app_motorControl_mode_E  selectedMode;                 // the method an enable runs; OFF leaves it selected
} app_motorControl_snapshot_S;

/* Public Function Declarations */

bool app_motorControl_init(const app_motorControl_config_S * const config);

// Copy out the channel's live mode/alignment/electrical-angle state. Returns
// false for an out-of-range channel, a NULL destination, or an uninitialized
// module, leaving the destination unchanged.
bool app_motorControl_getSnapshot(app_motorControl_channel_E channel, app_motorControl_snapshot_S * const snapshot);

void app_motorControl_run1ms(void);
void app_motorControl_run50us(void);

void app_motorControl_setVelocity(app_motorControl_channel_E channel, float32_t velocity_radPerSec);
void app_motorControl_setMode(app_motorControl_channel_E channel, app_motorControl_mode_E mode);

// Release a latched overcurrent fault (fw~safety_001). The drive stays disabled
// until re-enabled; a no-op for an out-of-range channel or an uninitialized
// module. This is the fault-clear action referenced by the trip latch — driven
// on-device by the user button's fault-clear gesture (fw~mc_007).
void app_motorControl_clearFault(app_motorControl_channel_E channel);
#ifdef __cplusplus
}
#endif
