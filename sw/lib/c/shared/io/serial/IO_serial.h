#ifndef IO_SERIAL_H
#define IO_SERIAL_H

/* Includes */
#include "lib_types.h"
#include "IO_serial_channels.h"   // IO_serial_channel_E

/* Typedefs */

// The transport backing a serial channel. USB CDC is the only one today; a
// UART would be the next.
typedef enum
{
    IO_SERIAL_TRANSPORT_USB_CDC,
    IO_SERIAL_TRANSPORT_COUNT,
} IO_serial_transport_E;

typedef struct
{
    IO_serial_transport_E transport;
} IO_serial_channelConfig_S;

typedef struct
{
    const IO_serial_channelConfig_S * channels;
    size_t numChannels;
} IO_serial_config_S;

/* Public Function Declarations */

bool IO_serial_init(const IO_serial_config_S * const config);

// Transmit len bytes on a channel, yielding while the backing transport is full
// and dropping bytes past a bounded retry limit so it never blocks indefinitely.
void IO_serial_write(IO_serial_channel_E channel, const uint8_t * bytes, uint32_t len);

// Number of received bytes available on a channel.
uint32_t IO_serial_available(IO_serial_channel_E channel);

// Read up to len received bytes from a channel into buffer; returns count read.
uint32_t IO_serial_read(IO_serial_channel_E channel, uint8_t * buffer, uint32_t len);

// True iff a channel's backing transport is connected.
bool IO_serial_connected(IO_serial_channel_E channel);

#endif // IO_SERIAL_H
