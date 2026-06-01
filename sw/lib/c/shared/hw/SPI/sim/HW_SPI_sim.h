#ifndef HW_SPI_SIM_H
#define HW_SPI_SIM_H

/* Includes */
#include "lib_types.h"

#include "HW_GPIO.h"
#include "HW_SPI.h"

/* Defines */

/* Typedefs */

/* Static Inline Functions */

/* Public Function Declarations */

// SIL-only control + inspection of the loopback SPI model. Lets native
// tests inject receive data, observe transmitted data and chip-select
// activity, force faults, and drive non-blocking completion — none of
// which has real hardware on the native target.

// Clear per-channel sim state (injection, capture, CS records, faults,
// pending transfers). Does not change the registered configuration.
void HW_SPI_sim_reset(void);

// Complete every pending non-blocking (interrupt/DMA) transfer: fill its
// receive buffer, deassert CS, set final status, and fire the channel's
// completion callback exactly once.
void HW_SPI_sim_tick(void);

// Bytes a subsequent receive() on `channel` will return (loopback aside).
void HW_SPI_sim_setInjectedRx(HW_SPI_channel_E channel, const uint8_t * data, size_t length);

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

#endif // HW_SPI_SIM_H
