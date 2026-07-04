#pragma once

#include "lib_types.h"
#include "IO_i2c_channels.h"

// Mock IO_i2c; an uninjected (device, register) reads back as zeros.

#define MOCK_IO_I2C_MAX_BYTES (8U)

void mock_IO_i2c_reset(void);
void mock_IO_i2c_setReg(IO_i2c_device_E dev, uint16_t reg, const uint8_t * bytes, size_t length);
void mock_IO_i2c_failReg(IO_i2c_device_E dev, uint16_t reg);
void mock_IO_i2c_failAll(bool fail);
size_t          mock_IO_i2c_readCount(void);
IO_i2c_device_E mock_IO_i2c_lastDevice(void);
