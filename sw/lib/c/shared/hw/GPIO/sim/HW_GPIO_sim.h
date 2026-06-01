#ifndef HW_GPIO_SIM_H
#define HW_GPIO_SIM_H

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

#endif // HW_GPIO_SIM_H
