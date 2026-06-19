#ifndef IO_AS5048_CHANNELS_H
#define IO_AS5048_CHANNELS_H

// Test-local channel seam (mirrors the project-provided header). Two
// encoders so addressing and per-channel reverse can be exercised.
typedef enum
{
    IO_AS5048_CHANNEL_ENC_A,
    IO_AS5048_CHANNEL_ENC_B,
    IO_AS5048_CHANNEL_COUNT,
} IO_AS5048_channel_E;

#endif // IO_AS5048_CHANNELS_H
