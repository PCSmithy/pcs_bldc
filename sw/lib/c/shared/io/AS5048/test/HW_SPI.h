#pragma once

// Minimal mock of the HW_SPI public header — only the surface IO_AS5048
// actually uses. The implementation lives in mock_HW_SPI.c and is driven by
// the controls in mock_HW_SPI.h.
#include "lib_types.h"
#include "HW_SPI_channels.h"

bool HW_SPI_transmitReceive(HW_SPI_channel_E channel, uint8_t * txData, uint8_t * rxData, size_t length);

