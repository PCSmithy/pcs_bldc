#ifndef HW_USB_H
#define HW_USB_H

/* Includes */
#include "lib_types.h"

/* Public Function Declarations */

// Bring up the USB device's CDC virtual-serial interface: route the USB clock,
// initialise the TinyUSB device stack, and enable the USB interrupt. Returns
// false if the device stack fails to initialise. Call once from main() before
// the scheduler starts, then service the stack from a dedicated task whose body
// calls HW_USB_run().
bool HW_USB_init(void);

// Service the USB device stack: block on the event queue until a USB event
// (posted by the USB ISR), process it, and push queued CDC TX toward the host.
// Intended as the body of a dedicated USB task; runs its own service loop and
// does not return under normal operation.
void HW_USB_run(void);

// True iff a host has opened the CDC virtual-serial port.
bool HW_USB_connected(void);

// Accept up to len bytes for transmission to the host; returns the count
// accepted (only what fits, zero when the transmit buffer is full).
uint32_t HW_USB_write(const uint8_t * data, uint32_t len);

// Hand accepted bytes to the device stack for delivery.
void HW_USB_writeFlush(void);

// Yield so the device-service task can drain the transmit buffer. Embedded
// blocks briefly; the sim is a no-op. Used by serial-layer backpressure.
void HW_USB_serviceYield(void);

// Number of received bytes available from the host.
uint32_t HW_USB_available(void);

// Read up to len received bytes into buffer; returns the count read.
uint32_t HW_USB_read(uint8_t * buffer, uint32_t len);

#endif // HW_USB_H
