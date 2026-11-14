#include "app_rotorEstimation.h"
#include "mock_app_rotorEstimation.h"

static float32_t mockAngle_rad;
static float32_t mockVelocity_radPerSec;
static float32_t mockUnmodeledTorque_Nm;
static float32_t mockTorqueInput_Nm;

void mock_app_rotorEstimation_reset(void)
{
    mockAngle_rad = 0.0f;
    mockVelocity_radPerSec = 0.0f;
    mockUnmodeledTorque_Nm = 0.0f;
    mockTorqueInput_Nm = 0.0f;
}

void mock_app_rotorEstimation_setAngle(float32_t angle_rad)
{
    mockAngle_rad = angle_rad;
}

void mock_app_rotorEstimation_setVelocity(float32_t velocity_radPerSec)
{
    mockVelocity_radPerSec = velocity_radPerSec;
}

void mock_app_rotorEstimation_setUnmodeledTorque(float32_t torque_Nm)
{
    mockUnmodeledTorque_Nm = torque_Nm;
}

float32_t mock_app_rotorEstimation_torqueInput(void)
{
    return mockTorqueInput_Nm;
}

/* ---- mocked app_rotorEstimation surface ---- */

void app_rotorEstimation_getState(app_rotorEstimation_channel_E channel, float32_t * theta, float32_t * omega, float32_t * unmodeledTorque)
{
    if (channel < APP_ROTORESTIMATION_CHANNEL_COUNT)
    {
        *theta = mockAngle_rad;
        *omega = mockVelocity_radPerSec;
        *unmodeledTorque = mockUnmodeledTorque_Nm;
    }
}

void app_rotorEstimation_setTorqueInput(app_rotorEstimation_channel_E channel, float32_t torque_Nm)
{
    (void)channel;
    mockTorqueInput_Nm = torque_Nm;
}
