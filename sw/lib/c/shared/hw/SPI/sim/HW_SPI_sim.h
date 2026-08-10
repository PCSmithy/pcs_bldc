#pragma once

/* Includes */
#include "lib_types.h"

#include "HW_GPIO.h"
#include "HW_SPI.h"

/* Defines */

/* Typedefs */

/* Static Inline Functions */

/* Public Function Declarations */

// SIL-only control of the SPI model. Lets native tests force faults and drive
// non-blocking completion — neither of which has real hardware on the native
// target. Receive data comes from the linked duplex peer over SIL_ports;
// chip-select activity is observed on the GPIO pins' observation ports.

// Complete every pending non-blocking (interrupt/DMA) transfer: fill its
// receive buffer, deassert CS, set final status, and fire the channel's
// completion callback exactly once.
void HW_SPI_sim_tick(void);

// When set, the next software transfer on `channel` reports a timeout
// (returns false, status ERROR) instead of completing.
void HW_SPI_sim_setStall(HW_SPI_channel_E channel, bool stall);

// When set, the next non-blocking transfer on `channel` completes with
// ERROR status instead of COMPLETE.
void HW_SPI_sim_setForceError(HW_SPI_channel_E channel, bool forceError);

