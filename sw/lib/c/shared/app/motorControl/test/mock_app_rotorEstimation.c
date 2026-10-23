#include "app_rotorEstimation.h"
#include "mock_app_rotorEstimation.h"

static float32_t mockAngle_rad;
static float32_t mockVelocity_radPerSec;

void mock_app_rotorEstimation_reset(void)
{
    mockAngle_rad = 0.0f;
    mockVelocity_radPerSec = 0.0f;
}

void mock_app_rotorEstimation_setAngle(float32_t angle_rad)
{
    mockAngle_rad = angle_rad;
}

void mock_app_rotorEstimation_setVelocity(float32_t velocity_radPerSec)
{
    mockVelocity_radPerSec = velocity_radPerSec;
}

/* ---- mocked app_rotorEstimation surface ---- */

void app_rotorEstimation_getState(app_rotorEstimation_channel_E channel, float32_t * theta, float32_t * omega)
{
    if (channel < APP_ROTORESTIMATION_CHANNEL_COUNT)
    {
        *theta = mockAngle_rad;
        *omega = mockVelocity_radPerSec;
    }
}
