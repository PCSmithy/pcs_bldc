#ifndef HW_SPI_H
#define HW_SPI_H

// Minimal mock of the HW_SPI public header — only the surface IO_SK6805
// uses (HW_SPI_transmit). Implemented by mock_HW_SPI.c, inspected via the
// controls in mock_HW_SPI.h.
#include "lib_types.h"
#include "HW_SPI_channels.h"

bool HW_SPI_transmit(HW_SPI_channel_E channel, uint8_t * txData, size_t length);

#endif // HW_SPI_H
