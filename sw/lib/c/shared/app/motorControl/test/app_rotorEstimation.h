#pragma once

// Boundary double of app_rotorEstimation — only the surface app_motorControl
// uses (the per-cycle angle and velocity). Implementation + controls in
// mock_app_rotorEstimation. Rationale: the real estimator moves 63 % of the
// way to a new angle per millisecond, so a test that places the rotor would
// have to wait out its settling; the double lets a test just set the angle.

#include "lib_types.h"
#include "app_rotorEstimation_channels.h"

#ifdef __cplusplus
extern "C" {
#endif

void app_rotorEstimation_getState(app_rotorEstimation_channel_E channel, float32_t * theta, float32_t * omega);

#ifdef __cplusplus
}
#endif
