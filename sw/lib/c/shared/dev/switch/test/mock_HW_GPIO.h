#ifndef MOCK_HW_GPIO_H
#define MOCK_HW_GPIO_H

#include "lib_types.h"
#include "HW_GPIO.h"

// Test controls for the mocked HW_GPIO cached-input snapshot. dev_switch reads
// pin levels through HW_GPIO_readCached; these drive what it sees.

// Clear all injected levels (every pin reads HW_GPIO_LEVEL_LOW).
void mock_HW_GPIO_reset(void);

// Set the cached level HW_GPIO_readCached returns for `pin` (single-bit mask)
// of `port`.
void mock_HW_GPIO_setCachedLevel(HW_GPIO_port_E port, uint32_t pin, HW_GPIO_level_E level);

#endif // MOCK_HW_GPIO_H
