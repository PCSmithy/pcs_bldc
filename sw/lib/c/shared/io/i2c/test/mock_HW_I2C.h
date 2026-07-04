#pragma once

#include "lib_types.h"
#include "HW_I2C.h"

// Test controls for the mocked HW_I2C. IO_i2c issues one memRead/memWrite per
// register op; the mock records that call's arguments, returns the injected
// success/failure, and (for reads) fills the caller's buffer with injected bytes.

#define MOCK_HW_I2C_MAX_BYTES (32U)

// The arguments IO_i2c passed to the most recent HW_I2C register transfer.
typedef struct
{
    bool called;
    bool isWrite;
    HW_I2C_bus_E bus;
    uint8_t devAddr7;
    uint16_t memAddr;
    HW_I2C_memAddrSize_E memAddrSize;
    size_t length;
    uint8_t data[MOCK_HW_I2C_MAX_BYTES];   // caller's bytes on a write
} mock_HW_I2C_call_S;

// Clear the recorded call, injected response, and force transfers to succeed.
void mock_HW_I2C_reset(void);

// Force the next (and subsequent) transfers to fail (false) or succeed (true).
void mock_HW_I2C_setTransferOk(bool ok);

// Bytes the mock returns in the caller's buffer on the next read.
void mock_HW_I2C_setResponse(const uint8_t * bytes, size_t length);

// The most recent register transfer's recorded arguments.
const mock_HW_I2C_call_S * mock_HW_I2C_lastCall(void);
