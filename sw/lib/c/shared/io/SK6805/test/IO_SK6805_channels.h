#pragma once

// Test-local channel seam: two LED-string channels so addressing and
// per-channel invert can be exercised. A small pixel count keeps the
// captured frames easy to decode.
typedef enum
{
    IO_SK6805_CHANNEL_A,
    IO_SK6805_CHANNEL_B,
    IO_SK6805_CHANNEL_COUNT,
} IO_SK6805_channel_E;

#define IO_SK6805_PIXEL_COUNT  4U

