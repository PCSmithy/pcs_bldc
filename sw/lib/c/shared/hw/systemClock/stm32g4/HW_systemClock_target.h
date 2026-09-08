#pragma once

// Target-specific half of HW_systemClock; reached via HW_systemClock.h.

/* Includes */
#include "lib_types.h"
#include "stm32g4xx_hal.h"

/* Typedefs */

typedef struct
{
    RCC_OscInitTypeDef RCC_OscInitStruct;
    RCC_ClkInitTypeDef RCC_ClkInitStruct;
    uint32_t PWREx_ControlVoltageScaling;
    uint32_t FLASH_latency;
} HW_systemClock_config_S;
