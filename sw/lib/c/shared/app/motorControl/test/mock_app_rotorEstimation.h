#pragma once

#include "lib_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Controls for the app_rotorEstimation boundary double: the mechanical angle
// and velocity getState reports for the one channel.

// Clear to 0 rad, 0 rad/s.
void mock_app_rotorEstimation_reset(void);

// Set the mechanical angle (radians) getState reports.
void mock_app_rotorEstimation_setAngle(float32_t angle_rad);

// Set the velocity (rad/s) getState reports.
void mock_app_rotorEstimation_setVelocity(float32_t velocity_radPerSec);

#ifdef __cplusplus
}
#endif
