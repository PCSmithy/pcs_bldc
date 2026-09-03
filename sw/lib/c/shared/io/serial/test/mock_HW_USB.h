#pragma once

/*
 * Kernel-free boundary mock of HW_USB.h for IO_serial's unit tests: a TX
 * capture, an RX queue, a connected flag, and a TX-accepting toggle.
 */

#include "lib_types.h"

void mock_HW_USB_reset(void);
void mock_HW_USB_setConnected(bool connected);
void mock_HW_USB_setTxAccepting(bool accepting);
uint32_t mock_HW_USB_txLen(void);
// Copy up to len captured TX bytes into out (non-consuming); returns the count.
uint32_t mock_HW_USB_readTx(uint8_t * out, uint32_t len);
void mock_HW_USB_injectRx(const uint8_t * bytes, uint32_t len);
