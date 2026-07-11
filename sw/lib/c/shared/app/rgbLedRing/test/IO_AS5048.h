#pragma once

// Minimal mock of the IO_AS5048 surface app_rgbLedRing uses (channel enum +
// cached-angle read). Implemented as a stub in mock_app_deps.c.
#include "lib_types.h"

typedef enum
{
    IO_AS5048_CHANNEL_MOTOR,
    IO_AS5048_CHANNEL_DIAL,
    IO_AS5048_CHANNEL_COUNT,
} IO_AS5048_channel_E;

bool IO_AS5048_readAngle(IO_AS5048_channel_E channel, uint16_t * angleRaw, float32_t * angle_deg, float32_t * angle_rad);

