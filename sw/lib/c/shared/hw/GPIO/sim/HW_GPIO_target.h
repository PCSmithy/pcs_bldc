#pragma once

// Target-specific half of HW_GPIO; reached via HW_GPIO.h.

/* Includes */
#include "lib_types.h"

/* Typedefs */

typedef enum
{
    HW_GPIO_MODE_INPUT,
    HW_GPIO_MODE_OUTPUT,
    HW_GPIO_MODE_INTERRUPT,
} HW_GPIO_mode_E;

// One pin. The sim has no electrical configuration to apply, so a pin carries
// its mask, its direction, and a human-readable name for trace logs.
typedef struct
{
    uint32_t       pin;        // single-bit (or multi-bit) GPIO_PIN_x mask
    HW_GPIO_mode_E mode;
    const char *   pinNameStr; // human-readable name for trace logs
} HW_GPIO_pinConfig_S;

typedef struct
{
    const HW_GPIO_pinConfig_S * pins;
    size_t numPins;
} HW_GPIO_portConfig_S;
