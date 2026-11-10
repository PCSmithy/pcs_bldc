#pragma once

// Shared test motor-control channel seam (mirrors the board's one channel) and
// the velocity ceiling the server hook validates against.
#define APP_MOTORCONTROL_MAX_VELOCITY_RAD_PER_SEC (20.0f)

typedef enum
{
    APP_MOTORCONTROL_CHANNEL_MAIN,
    APP_MOTORCONTROL_CHANNEL_COUNT,
} app_motorControl_channel_E;
