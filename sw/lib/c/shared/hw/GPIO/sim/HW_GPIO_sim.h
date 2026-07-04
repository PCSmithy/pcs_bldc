#pragma once

/* Includes */
#include "lib_types.h"

#include "HW_GPIO.h"

/* Defines */

/* Typedefs */

/* Static Inline Functions */

/* Public Function Declarations */

// SIL-only inspection of pin writes made through HW_GPIO_writePin.
// `pin` is a single-bit GPIO_PIN_x mask. Lets tests observe chip-select
// activity that has no real hardware on the native target.

// Last level driven onto `pin` of `port`; HW_GPIO_LEVEL_LOW before any write.
HW_GPIO_level_E HW_GPIO_sim_getLevel(HW_GPIO_port_E port, uint32_t pin);

// Number of HW_GPIO_writePin calls that touched `pin` of `port`.
uint32_t HW_GPIO_sim_getWriteCount(HW_GPIO_port_E port, uint32_t pin);

// Clear all recorded levels and write counts (call from test setUp).
void HW_GPIO_sim_reset(void);

// Inject the logical level captured for `pin` of `port` by the next
// HW_GPIO_run1ms (and read back via HW_GPIO_readCached). `pin` may carry
// multiple bits; each is set independently.
void HW_GPIO_sim_setInputLevel(HW_GPIO_port_E port, uint32_t pin, HW_GPIO_level_E level);

// Fire a signal edge on `pin` of `port`, invoking any registered EXTI
// callback once per set bit. No-op for bits with no registered callback.
void HW_GPIO_sim_triggerExti(HW_GPIO_port_E port, uint32_t pin);

