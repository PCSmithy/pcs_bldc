#pragma once

#include "lib_types.h"
#include "IO_AS5048_channels.h"

// Controls for the IO_AS5048 boundary double: set the radian angle each
// channel's readAngle returns.

// Clear all channels to 0 rad and status OK.
void mock_IO_AS5048_reset(void);

// Set the angle (radians) readAngle reports for `channel`.
void mock_IO_AS5048_setAngle(IO_AS5048_channel_E channel, float32_t angle_rad);

// Set the integrity status getStatus reports for `channel`.
void mock_IO_AS5048_setStatus(IO_AS5048_channel_E channel, IO_AS5048_status_E status);
