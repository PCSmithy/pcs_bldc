#pragma once

/* Includes */
#include "lib_types.h"
#include "lib_cobs.h"
#include "IO_COBSFrame_channels.h"   // IO_COBSFrame_channel_E + IO_COBSFRAME_MAX_PAYLOAD
#include "IO_serial.h"

/* Defines */

// CRC-32 trailer appended to each payload before COBS encoding.
#define IO_COBSFRAME_CRC_LEN  4U

// Worst-case wire size of one frame, including both 0x00 delimiters.
#define IO_COBSFRAME_WIRE_MAX(payloadLen) \
    (LIB_COBS_ENCODED_MAX((payloadLen) + IO_COBSFRAME_CRC_LEN) + 2U)

/* Typedefs */

typedef struct
{
    IO_serial_channel_E serialChannel;
    size_t maxFrameLen;   // largest decoded payload accepted; <= IO_COBSFRAME_MAX_PAYLOAD
} IO_COBSFrame_channelConfig_S;

typedef struct
{
    const IO_COBSFrame_channelConfig_S * channels;
    size_t numChannels;
} IO_COBSFrame_config_S;

/* Public Function Declarations */

bool IO_COBSFrame_init(const IO_COBSFrame_config_S * const config);

// Pump reception on every channel, holding at most one valid frame per channel
// until it is received; invalid or oversized segments are discarded.
void IO_COBSFrame_run(void);

// Copy the held frame into buffer and release it; false when none is held or
// buffer is too small (which drops the frame).
bool IO_COBSFrame_receive(IO_COBSFrame_channel_E channel, uint8_t * const buffer,
                      size_t bufferLen, size_t * const frameLen);

// Frame and transmit payload; false (nothing transmitted) when the whole
// encoded frame exceeds the channel's free transmit capacity.
bool IO_COBSFrame_send(IO_COBSFrame_channel_E channel, const uint8_t * const payload, size_t len);
