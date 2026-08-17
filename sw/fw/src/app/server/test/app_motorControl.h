#pragma once

// Shadow of app_motorControl.h for the app_server unit suite: the snapshot
// surface only, backed by mock_app_motorControl.c.

/* Includes */
#include "lib_types.h"

/* Typedefs */

typedef enum
{
    APP_MOTORCONTROL_CHANNEL_MAIN,
    APP_MOTORCONTROL_CHANNEL_COUNT,
} app_motorControl_channel_E;

typedef enum
{
    APP_MOTORCONTROL_MODE_OFF,
    APP_MOTORCONTROL_MODE_SIX_STEP_TRAP,
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
    float32_t                velocityMeasured_radPerSec;
} app_motorControl_snapshot_S;

/* Defines */

// Configured velocity ceiling the hook validates against.
#define APP_MOTORCONTROL_MAX_VELOCITY_RAD_PER_SEC (20.0f)

/* Public Function Declarations */

bool app_motorControl_getSnapshot(app_motorControl_channel_E channel,
                                  app_motorControl_snapshot_S * const snapshot);
void app_motorControl_setMode(app_motorControl_channel_E channel, app_motorControl_mode_E mode);
void app_motorControl_setVelocity(app_motorControl_channel_E channel, float32_t velocity_radPerSec);
void app_motorControl_clearFault(app_motorControl_channel_E channel);

/* Test controls */

void mock_app_motorControl_setSnapshot(const app_motorControl_snapshot_S * const snapshot);

// Setter recorders.
extern app_motorControl_mode_E mock_app_motorControl_lastMode;
extern float32_t               mock_app_motorControl_lastVelocity;
extern uint32_t                mock_app_motorControl_setModeCalls;
extern uint32_t                mock_app_motorControl_setVelocityCalls;
extern uint32_t                mock_app_motorControl_clearFaultCalls;
void mock_app_motorControl_resetRecorders(void);
