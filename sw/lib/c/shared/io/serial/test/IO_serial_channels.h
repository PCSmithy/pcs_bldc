#ifndef IO_SERIAL_CHANNELS_H
#define IO_SERIAL_CHANNELS_H

// Test-local channel seam: one CDC channel (the only transport today).
typedef enum
{
    IO_SERIAL_CHANNEL_CDC,
    IO_SERIAL_CHANNEL_COUNT,
} IO_serial_channel_E;

#endif // IO_SERIAL_CHANNELS_H
