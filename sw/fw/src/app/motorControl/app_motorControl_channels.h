#pragma once

/* Defines */
// What the drive holds under FOC on the 20 V bus: at 200 rpm (20.94 rad/s) the
// voltage ceiling tops the velocity loop out at 20.3-20.5 rad/s and a position
// move at that speed falls behind its profile (bench pos_trajectory, 2026-11-11).
#define APP_MOTORCONTROL_MAX_VELOCITY_RAD_PER_SEC (18.0f)

/* Typedefs */

typedef enum
{
    APP_MOTORCONTROL_CHANNEL_MAIN,
    APP_MOTORCONTROL_CHANNEL_COUNT,
} app_motorControl_channel_E;

