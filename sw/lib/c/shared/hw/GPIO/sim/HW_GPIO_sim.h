#pragma once

/* Includes */
#include "lib_types.h"

#include "HW_GPIO.h"

/* Defines */

/* Typedefs */

/* Static Inline Functions */

/* Public Function Declarations */

// SIL-only input injection for the sim GPIO model. Output pins are observed
// through their SIL_ports observation ports, not from here.

// Drop the config and clear injected input state, EXTI registrations, and
// output-port handles (call from test setUp).
void HW_GPIO_sim_reset(void);

// Inject the logical level captured for `pin` of `port` by the next
// HW_GPIO_run1ms (and read back via HW_GPIO_readCached). `pin` may carry
// multiple bits; each is set independently.
void HW_GPIO_sim_setInputLevel(HW_GPIO_port_E port, uint32_t pin, HW_GPIO_level_E level);

// Fire a signal edge on `pin` of `port`, invoking any registered EXTI
// callback once per set bit. No-op for bits with no registered callback.
void HW_GPIO_sim_triggerExti(HW_GPIO_port_E port, uint32_t pin);

