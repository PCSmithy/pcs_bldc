#pragma once

/* Includes */
#include "lib_types.h"

#include "HW_I2C.h"

/* Defines */

/* Typedefs */

/* Static Inline Functions */

/* Public Function Declarations */

// SIL-only control + inspection of the I2C bus/device model. Native tests have
// no real bus, so they inject register/receive bytes, observe transmitted and
// register bytes, and force faults through these hooks. All are keyed by the
// per-transfer bus + 7-bit device address, exactly as the transfer API is.

// Clear per-bus device state (register memory, captures, injection, faults).
// Does not change the registered configuration.
void HW_I2C_sim_reset(void);

// Bytes a subsequent receive() on (bus, devAddr7) will return.
void HW_I2C_sim_setInjectedRx(HW_I2C_bus_E bus, uint8_t devAddr7, const uint8_t * bytes, size_t length);

// Copy up to `maxLength` bytes most recently transmit()'d to (bus, devAddr7)
// into `out`; returns the full transmitted length.
size_t HW_I2C_sim_getLastTx(HW_I2C_bus_E bus, uint8_t devAddr7, uint8_t * out, size_t maxLength);

// Seed the device's register memory at `memAddr` (for memRead() tests).
void HW_I2C_sim_setRegBytes(HW_I2C_bus_E bus, uint8_t devAddr7, uint16_t memAddr,
                            const uint8_t * bytes, size_t length);

// Read back the device's register memory at `memAddr` (for memWrite() tests).
size_t HW_I2C_sim_getRegBytes(HW_I2C_bus_E bus, uint8_t devAddr7, uint16_t memAddr,
                              uint8_t * out, size_t length);

// When set, the next transfer on `bus` reports a timeout (returns false).
void HW_I2C_sim_setStall(HW_I2C_bus_E bus, bool stall);

// When set, the next transfer on `bus` reports an error (returns false).
void HW_I2C_sim_setForceError(HW_I2C_bus_E bus, bool forceError);
