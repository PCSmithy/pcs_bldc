#pragma once

/* Typedefs */

// Logical serial channels — one per byte stream the device exposes.
typedef enum
{
    IO_SERIAL_CHANNEL_CDC,   // USB CDC virtual COM port (to the desktop app)
    IO_SERIAL_CHANNEL_COUNT,
} IO_serial_channel_E;

