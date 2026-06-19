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
    // When true, output the complement (out = 360 - angle), reversing the
    // channel's apparent rotation direction. Use to match an encoder whose
    // mounting/sense runs opposite the desired convention.
    bool reverse;
} IO_AS5048_channelConfig_S;

typedef struct
{
    const IO_AS5048_channelConfig_S * channels;
    size_t numChannels;
} IO_AS5048_config_S;

// 14-bit absolute angle, so the raw reading spans 0..16383.
#define AS5048_COUNTS_PER_REV  16384U

// Per-channel outcome of the most recent _run1ms read.
// IDLE  = no read has completed yet.
// OK    = the last read passed parity and error-flag validation.
// FAULT = the last read failed parity, carried the error flag, or its SPI
//         transfer failed; the stored angle is held from the last good read.
typedef enum
{
    IO_AS5048_STATUS_IDLE,
    IO_AS5048_STATUS_OK,
    IO_AS5048_STATUS_FAULT,
} IO_AS5048_status_E;

/* Public Function Declarations */

bool IO_AS5048_init(const IO_AS5048_config_S * const config);
void IO_AS5048_run1ms(void);

// Read a channel's latest sampled angle (cached by _run1ms; non-blocking).
// Writes the raw count (0..16383) to *angleRaw and degrees to *angle_deg
// when each is non-NULL. Returns false if uninitialized or channel is out
// of range. The reading's integrity is reported separately by
// IO_AS5048_getStatus.
bool IO_AS5048_readAngle(IO_AS5048_channel_E channel, uint16_t * angleRaw, float32_t * angle_deg);

// Read the integrity status of a channel's most recent _run1ms read.
// Returns false if uninitialized, channel out of range, or out is NULL.
bool IO_AS5048_getStatus(IO_AS5048_channel_E channel, IO_AS5048_status_E * const out);

#endif // IO_AS5048_H
