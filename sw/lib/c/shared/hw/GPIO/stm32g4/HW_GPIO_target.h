#pragma once

// Target-specific half of HW_GPIO; reached via HW_GPIO.h.

/* Includes */
#include "lib_types.h"
#include "stm32g4xx_hal.h"

/* Typedefs */

typedef struct
{
    const GPIO_InitTypeDef * pins;
    size_t numPins;
} HW_GPIO_portConfig_S;
