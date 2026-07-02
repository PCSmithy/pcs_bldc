#ifndef HW_DMA_SIM_H
#define HW_DMA_SIM_H

/* Includes */
#include "lib_types.h"

#include "HW_DMA.h"

/* Public Function Declarations */

// SIL-only control + inspection of the loopback DMA model. Lets native tests
// observe memory-to-peripheral data, inject peripheral-to-memory data, force
// faults, and drive transfer completion — none of which has real hardware on
// the native target.

// Clear per-channel sim state (injection, capture, faults, pending transfers).
// Does not change the registered configuration.
void HW_DMA_sim_reset(void);

// Complete every pending transfer: for a peripheral-to-memory channel fill its
// memory buffer, set the final status, and fire the channel's completion
// callback exactly once.
void HW_DMA_sim_tick(void);

// Bytes a subsequent peripheral-to-memory transfer on `channel` will deliver
// into the caller's memory buffer.
void HW_DMA_sim_setInjectedPeriphData(HW_DMA_channel_E channel, const uint8_t * data, size_t length);

// Copy up to `maxLength` bytes most recently moved out of memory by a
// memory-to-peripheral transfer on `channel` into `out`; returns the full
// transferred length in bytes.
size_t HW_DMA_sim_getLastMemoryData(HW_DMA_channel_E channel, uint8_t * out, size_t maxLength);

// When set, the next transfer on `channel` completes with ERROR status instead
// of COMPLETE.
void HW_DMA_sim_setForceError(HW_DMA_channel_E channel, bool forceError);

// Number of transfers completed on `channel` since reset.
uint32_t HW_DMA_sim_getTransferCount(HW_DMA_channel_E channel);

#endif // HW_DMA_SIM_H
