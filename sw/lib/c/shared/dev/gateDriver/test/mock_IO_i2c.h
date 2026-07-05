#pragma once

#include "lib_types.h"
#include "IO_i2c_channels.h"

// Mock IO_i2c with read and write support. An uninjected (device, register)
// reads back as zeros; a successful write stores its bytes so a subsequent
// read returns them (readback-verify works against the mock's store).

#define MOCK_IO_I2C_MAX_BYTES  (8U)
#define MOCK_IO_I2C_MAX_WRITES (64U)

typedef struct
{
    IO_i2c_device_E dev;
    uint16_t        reg;
    uint8_t         value;   // first byte of the write
} mock_IO_i2c_write_S;

void mock_IO_i2c_reset(void);

// Seed a register's read-back bytes.
void mock_IO_i2c_setReg(IO_i2c_device_E dev, uint16_t reg, const uint8_t * bytes, size_t length);

// Reads of (dev, reg) fail.
void mock_IO_i2c_failReg(IO_i2c_device_E dev, uint16_t reg);

// Writes to (dev, reg) fail (and store nothing).
void mock_IO_i2c_failWriteReg(IO_i2c_device_E dev, uint16_t reg);

// Writes to (dev, reg) report success but leave the stored bytes unchanged —
// simulates a register whose readback disagrees with the write.
void mock_IO_i2c_stickReg(IO_i2c_device_E dev, uint16_t reg, const uint8_t * bytes, size_t length);

// All reads and writes fail.
void mock_IO_i2c_failAll(bool fail);

size_t mock_IO_i2c_readCount(void);
size_t mock_IO_i2c_writeCount(void);

// Copy up to maxWrites of the in-order write log into out; returns the number
// of writes logged (attempts, including failed ones).
size_t mock_IO_i2c_getWrites(mock_IO_i2c_write_S * out, size_t maxWrites);
