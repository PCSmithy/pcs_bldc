#pragma once

/* Includes */
#include "lib_types.h"

// Mirror of the stm32g4 HW_USB API. The sim is a loopback: init is a no-op
// success, connection/transmit/receive are driven by the SIL hooks in
// HW_USB_sim.h so consumers (IO_serial) can be exercised on the native target.

/* Public Function Declarations */

bool HW_USB_init(void);
void HW_USB_run(void);
bool HW_USB_connected(void);
uint32_t HW_USB_write(const uint8_t * data, uint32_t len);
void HW_USB_writeFlush(void);
void HW_USB_serviceYield(void);
uint32_t HW_USB_available(void);
uint32_t HW_USB_read(uint8_t * buffer, uint32_t len);

