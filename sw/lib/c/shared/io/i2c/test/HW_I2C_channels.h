#pragma once

// Test-local I2C bus seam. The mock HW_I2C records which bus each transfer
// targets, keyed on this enum.
typedef enum
{
    HW_I2C_BUS_A,
    HW_I2C_BUS_B,
    HW_I2C_BUS_COUNT,
} HW_I2C_bus_E;
