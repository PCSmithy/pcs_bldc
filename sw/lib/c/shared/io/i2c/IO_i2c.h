#pragma once

/* Includes */
#include "lib_types.h"
#include "IO_i2c_channels.h"
#include "HW_I2C.h"

/* Defines */

typedef struct
{
    HW_I2C_bus_E bus;
    uint8_t devAddr7;
    HW_I2C_memAddrSize_E memAddrSize;
} IO_i2c_deviceConfig_S;

typedef struct
{
    const IO_i2c_deviceConfig_S * devices;
    size_t numDevices;
} IO_i2c_config_S;

/* Public Function Declarations */

bool IO_i2c_init(const IO_i2c_config_S * const config);

// Register read/write addressed by logical device. The device's bus, 7-bit
// address, and register-offset width all come from config; `reg` is the
// register offset and `buffer`/`length` the payload. Returns false if
// uninitialized, `dev` is out of range, or the HW_I2C transfer fails.
bool IO_i2c_readReg(IO_i2c_device_E dev, uint16_t reg, uint8_t * buffer, size_t length);
bool IO_i2c_writeReg(IO_i2c_device_E dev, uint16_t reg, uint8_t * buffer, size_t length);
