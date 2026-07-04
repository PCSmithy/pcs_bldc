#pragma once

// Test-local IO_i2c: register entry points + device enum only, omitting the
// HW_I2C/HAL config so the driver builds in isolation.

#include "lib_types.h"
#include "IO_i2c_channels.h"

bool IO_i2c_readReg(IO_i2c_device_E dev, uint16_t reg, uint8_t * buffer, size_t length);
bool IO_i2c_writeReg(IO_i2c_device_E dev, uint16_t reg, uint8_t * buffer, size_t length);
