#pragma once

/* Includes */
#include "lib_types.h"

#include "HW_GPIO.h"
#include "HW_SPI.h"

/* Defines */

/* Typedefs */

/* Static Inline Functions */

/* Public Function Declarations */

// SIL-only control + inspection of the SPI model. Lets native tests observe
// transmitted data and chip-select activity, force faults, and drive
// non-blocking completion — none of which has real hardware on the native
// target. Receive data comes from the linked duplex peer over SIL_ports.

// Complete every pending non-blocking (interrupt/DMA) transfer: fill its
// receive buffer, deassert CS, set final status, and fire the channel's
// completion callback exactly once.
void HW_SPI_sim_tick(void);

// Copy up to `maxLength` bytes most recently transmitted on `channel`
// into `out`; returns the full transmitted length.
size_t HW_SPI_sim_getLastTx(HW_SPI_channel_E channel, uint8_t * out, size_t maxLength);

// When set, the next software transfer on `channel` reports a timeout
// (returns false, status ERROR) instead of completing.
void HW_SPI_sim_setStall(HW_SPI_channel_E channel, bool stall);

// When set, the next non-blocking transfer on `channel` completes with
// ERROR status instead of COMPLETE.
void HW_SPI_sim_setForceError(HW_SPI_channel_E channel, bool forceError);

// CS activity recorded during the last transfer on `channel`.
uint32_t        HW_SPI_sim_getCsAssertCount(HW_SPI_channel_E channel);
HW_GPIO_level_E HW_SPI_sim_getCsAssertLevel(HW_SPI_channel_E channel);
HW_GPIO_level_E HW_SPI_sim_getCsDeassertLevel(HW_SPI_channel_E channel);

