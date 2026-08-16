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
