#pragma once

// Boundary double of IO_AS5048 — only the surface app_motorControl uses
// (readAngle's radian output). Implementation + controls in mock_IO_AS5048.
// Rationale: injecting a rotor angle through the real SPI decode chain would
// mean hand-crafting AS5048 frames; the double lets a test just set an angle.

#include "lib_types.h"
#include "IO_AS5048_channels.h"

typedef enum
{
    IO_AS5048_STATUS_IDLE,
    IO_AS5048_STATUS_OK,
    IO_AS5048_STATUS_FAULT,
} IO_AS5048_status_E;

bool IO_AS5048_readAngle(IO_AS5048_channel_E channel, uint16_t * angleRaw, float32_t * angle_deg, float32_t * angle_rad);
bool IO_AS5048_getStatus(IO_AS5048_channel_E channel, IO_AS5048_status_E * const out);
