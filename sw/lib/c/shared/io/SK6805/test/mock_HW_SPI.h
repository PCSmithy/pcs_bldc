#ifndef MOCK_HW_SPI_H
#define MOCK_HW_SPI_H

#include "lib_types.h"
#include "HW_SPI_channels.h"

// Test controls for the mocked HW_SPI. Each HW_SPI_transmit captures the
// transmitted bytes per channel; the test reads them back to decode the
// emitted SK6805 frame.

void mock_HW_SPI_reset(void);

// Force `channel`'s transmit to fail (return false).
void mock_HW_SPI_setTransferOk(HW_SPI_channel_E channel, bool ok);

// The bytes / length of the most recent transmit on `channel`.
const uint8_t * mock_HW_SPI_txBuf(HW_SPI_channel_E channel);
size_t          mock_HW_SPI_txLen(HW_SPI_channel_E channel);

#endif // MOCK_HW_SPI_H
