#ifndef IO_SK6805_H
#define IO_SK6805_H

// Minimal mock of the IO_SK6805 surface app_rgbLedRing uses (channel enum +
// stage/transmit API). Implemented as no-op stubs in mock_app_deps.c.
#include "lib_types.h"

typedef enum
{
    IO_SK6805_CHANNEL_RING,
    IO_SK6805_CHANNEL_COUNT,
} IO_SK6805_channel_E;

void IO_SK6805_setPixel(IO_SK6805_channel_E channel, uint16_t index, uint8_t red, uint8_t green, uint8_t blue);
void IO_SK6805_setAll(IO_SK6805_channel_E channel, uint8_t red, uint8_t green, uint8_t blue);
void IO_SK6805_clear(IO_SK6805_channel_E channel);
bool IO_SK6805_update(IO_SK6805_channel_E channel);

#endif // IO_SK6805_H
