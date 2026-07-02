#ifndef HW_GPIO_H
#define HW_GPIO_H

// Minimal mock of the HW_GPIO public header — only the surface dev_switch
// actually uses (port/level types + the cached-input read). The implementation
// lives in mock_HW_GPIO.c and is driven by the controls in mock_HW_GPIO.h.
#include "lib_types.h"

typedef enum
{
    HW_GPIO_PORT_A,
    HW_GPIO_PORT_B,
    HW_GPIO_PORT_C,
    HW_GPIO_PORT_D,
    HW_GPIO_PORT_E,
    HW_GPIO_PORT_F,
    HW_GPIO_PORT_G,

    HW_GPIO_PORT_COUNT,
} HW_GPIO_port_E;

typedef enum
{
    HW_GPIO_LEVEL_LOW,
    HW_GPIO_LEVEL_HIGH,
} HW_GPIO_level_E;

// Cached level of input `pin` (single-bit GPIO_PIN_x mask) of `port`;
// HW_GPIO_LEVEL_LOW until a value is injected via mock_HW_GPIO_setCachedLevel.
HW_GPIO_level_E HW_GPIO_readCached(HW_GPIO_port_E port, uint32_t pin);

#endif // HW_GPIO_H
