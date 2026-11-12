#pragma once

// Boundary double of app_motorControl: the consumer surface (snapshot, setters,
// fault clear). Implementation + controls in mock_app_motorControl.

#include "lib_types.h"
#include "app_motorControl_channels.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    APP_MOTORCONTROL_MODE_OFF,
    APP_MOTORCONTROL_MODE_SIX_STEP_TRAP,
    APP_MOTORCONTROL_MODE_SINUSOIDAL,
    APP_MOTORCONTROL_MODE_FOC_TORQUE,
    APP_MOTORCONTROL_MODE_FOC_VELOCITY,
    APP_MOTORCONTROL_MODE_FOC_POSITION,
    APP_MOTORCONTROL_MODE_COUNT,
} app_motorControl_mode_E;

typedef enum
{
    APP_MOTORCONTROL_STATE_DISABLED,
    APP_MOTORCONTROL_STATE_ENABLED,
    APP_MOTORCONTROL_STATE_FAULTED,
    APP_MOTORCONTROL_STATE_COUNT,
} app_motorControl_state_E;

typedef struct
{
    app_motorControl_mode_E  mode;
    app_motorControl_state_E state;
    bool                     isAligned;
    float32_t                magneticAngle_rad;
    float32_t                velocitySetpoint_radPerSec;
    float32_t                torqueTarget_Nm;
    float32_t                velocityMeasured_radPerSec;
    float32_t                positionTarget_rad;
    float32_t                profileVelocity_radPerSec;
    float32_t                rotorPosition_rad;
    app_motorControl_mode_E  selectedMode;
    uint16_t                 faultLatches;
} app_motorControl_snapshot_S;

typedef struct
{
    float32_t torqueConstant_NmPerA;
    float32_t maxCurrent_A;
    float32_t maxTorque_Nm;
    float32_t maxVelocity_radPerSec;
    float32_t maxPosition_rad;
} app_motorControl_ratings_S;

bool app_motorControl_getRatings(app_motorControl_channel_E channel, app_motorControl_ratings_S * const ratings);
bool app_motorControl_getSnapshot(app_motorControl_channel_E channel, app_motorControl_snapshot_S * const snapshot);
void app_motorControl_setMode(app_motorControl_channel_E channel, app_motorControl_mode_E mode);
void app_motorControl_setVelocity(app_motorControl_channel_E channel, float32_t velocity_radPerSec, float32_t rateLimit_radPerSec2);
void app_motorControl_setTorque(app_motorControl_channel_E channel, float32_t torque_Nm, float32_t rateLimit_NmPerSec);
void app_motorControl_setPositionSetpoint(app_motorControl_channel_E channel, float32_t position_rad, float32_t velocityLimit_radPerSec,
                                          float32_t accelLimit_radPerSec2, float32_t velocityAtPoint_radPerSec);
void app_motorControl_clearFault(app_motorControl_channel_E channel);

#ifdef __cplusplus
}
#endif
