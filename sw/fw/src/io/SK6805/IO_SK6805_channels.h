#pragma once

/* Typedefs */

// Logical SK6805 LED-string channels — one per physical string.
typedef enum
{
    IO_SK6805_CHANNEL_RING,   // 36-LED status ring (D8..D43), SPI3 MOSI (PB5)
    IO_SK6805_CHANNEL_COUNT,
} IO_SK6805_channel_E;

/* Defines */

// LEDs per string. Shared across channels (the board has one 36-LED ring).
// See hw/rgb_LEDs.kicad_sch.
#define IO_SK6805_PIXEL_COUNT  36U

