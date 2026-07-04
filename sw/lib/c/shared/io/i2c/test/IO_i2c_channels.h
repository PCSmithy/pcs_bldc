#pragma once

// Test-local device seam: two logical devices so the suite can map both to a
// single bus at distinct addresses (fw~io_i2c_002) and give each a different
// register-offset width (fw~io_i2c_003).
typedef enum
{
    IO_I2C_DEVICE_0,
    IO_I2C_DEVICE_1,
    IO_I2C_DEVICE_COUNT,
} IO_i2c_device_E;
