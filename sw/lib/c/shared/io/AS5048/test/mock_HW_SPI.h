#pragma once

#include "lib_types.h"
#include "HW_SPI_channels.h"

// Test controls for the mocked HW_SPI. The driver issues a 16-bit big-endian
// transfer per frame; the mock returns the channel's injected response and
// records the last command word.

// Clear all injected responses (response 0, transfers succeed).
void mock_HW_SPI_reset(void);

// Set the 16-bit response frame the mock returns for `channel` on its next
// (and subsequent) transmitReceive calls.
void mock_HW_SPI_setResponse(HW_SPI_channel_E channel, uint16_t frame);

// Force `channel`'s transfers to fail (return false), modelling an SPI fault.
void mock_HW_SPI_setTransferOk(HW_SPI_channel_E channel, bool ok);

// The last command word the driver clocked out on `channel`.
uint16_t mock_HW_SPI_lastCommand(HW_SPI_channel_E channel);

