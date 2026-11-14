#pragma once

#include "lib_types.h"
#include "app_motorControl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Controls for the app_motorControl boundary double: the snapshot getSnapshot
// returns, and recorders of the last value and call count behind each setter.

// Clear the snapshot to zero and every recorder.
void mock_app_motorControl_reset(void);
void mock_app_motorControl_setSnapshot(const app_motorControl_snapshot_S * const snapshot);
void mock_app_motorControl_setRatings(const app_motorControl_ratings_S * const ratings);

extern app_motorControl_mode_E mock_app_motorControl_lastMode;
extern float32_t               mock_app_motorControl_lastVelocity;
extern float32_t               mock_app_motorControl_lastVelocityLimit;
extern float32_t               mock_app_motorControl_lastTorque;
extern float32_t               mock_app_motorControl_lastTorqueLimit;
extern float32_t               mock_app_motorControl_lastPosition;
extern float32_t               mock_app_motorControl_lastPositionVelocityLimit;
extern float32_t               mock_app_motorControl_lastPositionAccelLimit;
extern float32_t               mock_app_motorControl_lastPositionVelocityAtPoint;
extern uint32_t                mock_app_motorControl_setModeCalls;
extern uint32_t                mock_app_motorControl_setVelocityCalls;
extern uint32_t                mock_app_motorControl_setTorqueCalls;
extern uint32_t                mock_app_motorControl_setPositionSetpointCalls;
extern uint32_t                mock_app_motorControl_clearFaultCalls;

#ifdef __cplusplus
}
#endif
