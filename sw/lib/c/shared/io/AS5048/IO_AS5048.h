#ifndef IO_AS5048_H
#define IO_AS5048_H

/* Includes */
#include "lib_types.h"
#include "IO_AS5048_channels.h"
#include "HW_SPI_channels.h"

/* Defines */

typedef struct
{
    HW_SPI_channel_E spiChannel;
} IO_AS5048_channelConfig_S;

typedef struct
{
    const IO_AS5048_channelConfig_S * channels;
    size_t numChannels;
} IO_AS5048_config_S;

// 14-bit absolute angle, so the raw reading spans 0..16383.
#define AS5048_COUNTS_PER_REV  16384U

/* Public Function Declarations */

bool IO_AS5048_init(const IO_AS5048_config_S * const config);
void IO_AS5048_run1ms(void);

// Read the 14-bit absolute angle, blocking.
// On success returns true and writes the raw count (0..16383) to
// *angleRaw; returns false on SPI failure or if the AS5048 flags an error.
bool IO_AS5048_readAngle(IO_AS5048_channel_E channel,uint16_t * angleRaw, float32_t * angle_deg);

#endif // IO_AS5048_H
