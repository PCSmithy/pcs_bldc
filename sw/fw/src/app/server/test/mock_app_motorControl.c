/* Includes */
#include "app_motorControl.h"

/* Private Data Definitions */

static app_motorControl_snapshot_S mock_snapshot;

/* Public Function Definitions */

bool app_motorControl_getSnapshot(app_motorControl_channel_E channel,
                                  app_motorControl_snapshot_S * const snapshot)
{
    bool ret = false;
    if ((channel < APP_MOTORCONTROL_CHANNEL_COUNT) && (snapshot != NULL))
    {
        *snapshot = mock_snapshot;
        ret = true;
    }
    return ret;
}

void mock_app_motorControl_setSnapshot(const app_motorControl_snapshot_S * const snapshot)
{
    if (snapshot != NULL)
    {
        mock_snapshot = *snapshot;
    }
}

app_motorControl_mode_E mock_app_motorControl_lastMode;
float32_t               mock_app_motorControl_lastVelocity;
uint32_t                mock_app_motorControl_setModeCalls;
uint32_t                mock_app_motorControl_setVelocityCalls;
uint32_t                mock_app_motorControl_clearFaultCalls;

void app_motorControl_setMode(app_motorControl_channel_E channel, app_motorControl_mode_E mode)
{
    (void) channel;
    mock_app_motorControl_lastMode = mode;
    mock_app_motorControl_setModeCalls++;
}

void app_motorControl_setVelocity(app_motorControl_channel_E channel, float32_t velocity_radPerSec)
{
    (void) channel;
    mock_app_motorControl_lastVelocity = velocity_radPerSec;
    mock_app_motorControl_setVelocityCalls++;
}

void app_motorControl_clearFault(app_motorControl_channel_E channel)
{
    (void) channel;
    mock_app_motorControl_clearFaultCalls++;
}

void mock_app_motorControl_resetRecorders(void)
{
    mock_app_motorControl_lastMode = APP_MOTORCONTROL_MODE_OFF;
    mock_app_motorControl_lastVelocity = 0.0f;
    mock_app_motorControl_setModeCalls = 0U;
    mock_app_motorControl_setVelocityCalls = 0U;
    mock_app_motorControl_clearFaultCalls = 0U;
}
