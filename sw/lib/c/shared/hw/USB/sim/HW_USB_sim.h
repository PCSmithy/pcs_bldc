#pragma once

/* Includes */
#include "lib_types.h"

/* SIL inspection / control API — native sim target only. */

// Reset all loopback state (disconnected, TX accepting, buffers empty).
void HW_USB_sim_reset(void);

// Set whether a host is "connected" (drives HW_USB_connected).
void HW_USB_sim_setConnected(bool connected);

// When false, HW_USB_write accepts nothing (models a full TX buffer) so
// backpressure can be exercised. Defaults to true.
void HW_USB_sim_setTxAccepting(bool accepting);

// Read up to len bytes the consumer has transmitted into the loopback; returns
// the count copied. Does not consume.
uint32_t HW_USB_sim_readTx(uint8_t * buffer, uint32_t len);

// Total bytes transmitted into the loopback since reset.
uint32_t HW_USB_sim_txLen(void);

// Inject bytes as if received from the host (readable via HW_USB_read).
void HW_USB_sim_injectRx(const uint8_t * data, uint32_t len);

