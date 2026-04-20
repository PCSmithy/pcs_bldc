
#ifndef HW_SYSTEM_CLOCK_H
#define HW_SYSTEM_CLOCK_H

/* Includes */
#include "lib_types.h"

#include "stm32g4xx_hal.h"

#include "HW_systemClock_config.h"

/* Defines */

/* Typedefs */
typedef struct
{
    RCC_OscInitTypeDef RCC_OscInitStruct;
    RCC_ClkInitTypeDef RCC_ClkInitStruct;
    uint32_t PWREx_ControlVoltageScaling;
    uint32_t FLASH_latency;
} HW_systemClock_config_S;

/* Static Inline Functions */

/* Public Function Declarations */
bool HW_systemClock_init(const HW_systemClock_config_S * const config);

#endif // HW_SYSTEM_CLOCK_H