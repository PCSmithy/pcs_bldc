#pragma once

// Test-local AS5048 channel seam (mirrors the board's two encoders).
typedef enum
{
    IO_AS5048_CHANNEL_MOTOR,
    IO_AS5048_CHANNEL_DIAL,
    IO_AS5048_CHANNEL_COUNT,
} IO_AS5048_channel_E;
