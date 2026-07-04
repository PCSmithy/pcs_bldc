#pragma once

// Minimal mock of the HW_I2C public header — only the surface IO_i2c actually
// uses. The register-transfer implementations live in mock_HW_I2C.c and are
// driven by the controls in mock_HW_I2C.h.

#include "lib_types.h"
#include "HW_I2C_channels.h"

// Register-offset width for HW_I2C_memRead / HW_I2C_memWrite.
typedef enum
{
    HW_I2C_MEMADDR_SIZE_8BIT,
    HW_I2C_MEMADDR_SIZE_16BIT,
} HW_I2C_memAddrSize_E;

bool HW_I2C_memRead(HW_I2C_bus_E bus, uint8_t devAddr7, uint16_t memAddr,
                    HW_I2C_memAddrSize_E memAddrSize, uint8_t * data, size_t length);
bool HW_I2C_memWrite(HW_I2C_bus_E bus, uint8_t devAddr7, uint16_t memAddr,
                     HW_I2C_memAddrSize_E memAddrSize, uint8_t * data, size_t length);
