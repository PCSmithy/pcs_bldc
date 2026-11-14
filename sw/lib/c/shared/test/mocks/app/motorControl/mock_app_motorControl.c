#include "app_motorControl.h"
#include "mock_app_motorControl.h"

static app_motorControl_snapshot_S mockSnapshot;
static app_motorControl_ratings_S  mockRatings;

app_motorControl_mode_E mock_app_motorControl_lastMode;
float32_t               mock_app_motorControl_lastVelocity;
float32_t               mock_app_motorControl_lastVelocityLimit;
float32_t               mock_app_motorControl_lastTorque;
float32_t               mock_app_motorControl_lastTorqueLimit;
float32_t               mock_app_motorControl_lastPosition;
float32_t               mock_app_motorControl_lastPositionVelocityLimit;
float32_t               mock_app_motorControl_lastPositionAccelLimit;
float32_t               mock_app_motorControl_lastPositionVelocityAtPoint;
uint32_t                mock_app_motorControl_setModeCalls;
uint32_t                mock_app_motorControl_setVelocityCalls;
uint32_t                mock_app_motorControl_setTorqueCalls;
uint32_t                mock_app_motorControl_setPositionSetpointCalls;
uint32_t                mock_app_motorControl_clearFaultCalls;

void mock_app_motorControl_setSnapshot(const app_motorControl_snapshot_S * const snapshot)
{
    if (snapshot != NULL)
    {
        mockSnapshot = *snapshot;
    }
}

void mock_app_motorControl_setRatings(const app_motorControl_ratings_S * const ratings)
{
    if (ratings != NULL)
    {
        mockRatings = *ratings;
    }
}

void mock_app_motorControl_reset(void)
{
    mockSnapshot = (app_motorControl_snapshot_S){ 0 };
    mockRatings = (app_motorControl_ratings_S){ 0 };
    mock_app_motorControl_lastMode = APP_MOTORCONTROL_MODE_OFF;
    mock_app_motorControl_lastVelocity = 0.0f;
    mock_app_motorControl_lastVelocityLimit = 0.0f;
    mock_app_motorControl_lastTorque = 0.0f;
    mock_app_motorControl_lastTorqueLimit = 0.0f;
    mock_app_motorControl_lastPosition = 0.0f;
    mock_app_motorControl_lastPositionVelocityLimit = 0.0f;
    mock_app_motorControl_lastPositionAccelLimit = 0.0f;
    mock_app_motorControl_lastPositionVelocityAtPoint = 0.0f;
    mock_app_motorControl_setModeCalls = 0U;
    mock_app_motorControl_setVelocityCalls = 0U;
    mock_app_motorControl_setTorqueCalls = 0U;
    mock_app_motorControl_setPositionSetpointCalls = 0U;
    mock_app_motorControl_clearFaultCalls = 0U;
}

/* ---- mocked app_motorControl surface ---- */

bool app_motorControl_getRatings(app_motorControl_channel_E channel, app_motorControl_ratings_S * const ratings)
{
    bool ret = false;
    if ((channel < APP_MOTORCONTROL_CHANNEL_COUNT) && (ratings != NULL))
    {
        *ratings = mockRatings;
        ret = true;
    }
    return ret;
}

bool app_motorControl_getSnapshot(app_motorControl_channel_E channel, app_motorControl_snapshot_S * const snapshot)
{
    bool ret = false;
    if ((channel < APP_MOTORCONTROL_CHANNEL_COUNT) && (snapshot != NULL))
    {
        *snapshot = mockSnapshot;
        ret = true;
    }
    return ret;
}

void app_motorControl_setMode(app_motorControl_channel_E channel, app_motorControl_mode_E mode)
{
    (void) channel;
    mock_app_motorControl_lastMode = mode;
    mock_app_motorControl_setModeCalls++;
}

void app_motorControl_setVelocity(app_motorControl_channel_E channel, float32_t velocity_radPerSec, float32_t rateLimit_radPerSec2)
{
    (void) channel;
    mock_app_motorControl_lastVelocity = velocity_radPerSec;
    mock_app_motorControl_lastVelocityLimit = rateLimit_radPerSec2;
    mock_app_motorControl_setVelocityCalls++;
}

void app_motorControl_setTorque(app_motorControl_channel_E channel, float32_t torque_Nm, float32_t rateLimit_NmPerSec)
{
    (void) channel;
    mock_app_motorControl_lastTorque = torque_Nm;
    mock_app_motorControl_lastTorqueLimit = rateLimit_NmPerSec;
    mock_app_motorControl_setTorqueCalls++;
}

void app_motorControl_setPositionSetpoint(app_motorControl_channel_E channel, float32_t position_rad, float32_t velocityLimit_radPerSec,
                                          float32_t accelLimit_radPerSec2, float32_t velocityAtPoint_radPerSec)
{
    (void) channel;
    mock_app_motorControl_lastPosition = position_rad;
    mock_app_motorControl_lastPositionVelocityLimit = velocityLimit_radPerSec;
    mock_app_motorControl_lastPositionAccelLimit = accelLimit_radPerSec2;
    mock_app_motorControl_lastPositionVelocityAtPoint = velocityAtPoint_radPerSec;
    mock_app_motorControl_setPositionSetpointCalls++;
}

void app_motorControl_clearFault(app_motorControl_channel_E channel)
{
    (void) channel;
    mock_app_motorControl_clearFaultCalls++;
}
