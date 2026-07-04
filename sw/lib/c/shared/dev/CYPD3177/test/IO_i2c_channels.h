#pragma once

// Test-local device seam; two devices exercise per-channel independence.
typedef enum
{
    IO_I2C_DEVICE_0,
    IO_I2C_DEVICE_1,
    IO_I2C_DEVICE_COUNT,
} IO_i2c_device_E;
