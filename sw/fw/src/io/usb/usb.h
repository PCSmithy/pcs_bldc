#ifndef IO_USB_H
#define IO_USB_H

#include "lib_types.h"

// Bring up the USB device stack (TinyUSB CDC / virtual COM port): routes the
// 48 MHz USB clock, enables the USB_LP interrupt, and spawns the FreeRTOS task
// that services the device stack. Call once from main() before the scheduler
// starts. STM32G4 (embedded) target only. Returns false if the device task
// could not be created (e.g. FreeRTOS heap exhaustion).
bool USB_init(void);

#endif // IO_USB_H
