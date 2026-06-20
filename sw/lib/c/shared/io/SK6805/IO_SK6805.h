#ifndef IO_SK6805_H
#define IO_SK6805_H

/* Includes */
#include "lib_types.h"
#include "HW_SPI_channels.h"      // HW_SPI_channel_E
#include "IO_SK6805_channels.h"   // IO_SK6805_channel_E + IO_SK6805_PIXEL_COUNT

/* Defines */

// SK6805 wire encoding: one LED string driven from SPI MOSI, each color bit
// expanded to 6 SPI bits at 4.5 MHz (see HW_SPI BUS_3 config). 24 bits/pixel
// in GRB order; a trailing low gap latches the frame (>=80us reset).
#define SK6805_COLORS_PER_PIXEL    3U   // G, R, B
#define SK6805_SPI_BYTES_PER_COLOR 6U   // 8 color bits * 6 SPI bits / 8
#define SK6805_RESET_BYTES         50U  // >=80us low; 1 SPI byte ~= 1.78us @ 4.5 MHz

#define IO_SK6805_TXBUF_BYTES \
    (((IO_SK6805_PIXEL_COUNT) * SK6805_COLORS_PER_PIXEL * SK6805_SPI_BYTES_PER_COLOR) \
     + SK6805_RESET_BYTES)

/* Typedefs */

typedef struct
{
    HW_SPI_channel_E spiChannel;
    // Set when the data line passes through an inverting level shifter: the
    // driver then complements MOSI so the SK6805 wire sees the correct
    // waveform (and the reset gap is driven low on the wire).
    bool invert;
} IO_SK6805_channelConfig_S;

typedef struct
{
    const IO_SK6805_channelConfig_S * channels;
    size_t numChannels;
} IO_SK6805_config_S;

/* Public Function Declarations */

bool IO_SK6805_init(const IO_SK6805_config_S * const config);

// Stage a pixel colour in a channel's framebuffer (0-based). Out-of-range
// channel or index is a no-op. Not sent to the string until IO_SK6805_update().
void IO_SK6805_setPixel(IO_SK6805_channel_E channel, uint16_t index, uint8_t red, uint8_t green, uint8_t blue);
void IO_SK6805_setAll(IO_SK6805_channel_E channel, uint8_t red, uint8_t green, uint8_t blue);
void IO_SK6805_clear(IO_SK6805_channel_E channel);

// Expand a channel's framebuffer into the SPI bit pattern and blast it out
// (blocking). Returns false if not initialized, the channel is out of range,
// or the SPI transfer fails.
bool IO_SK6805_update(IO_SK6805_channel_E channel);

#endif // IO_SK6805_H
