
#ifndef HW_SYSTEM_CLOCK_H
#define HW_SYSTEM_CLOCK_H

/* Includes */
#include "lib_types.h"

#include "HW_systemClock_config.h"

/* Defines */

/* Typedefs */
typedef struct
{
    uint8_t _void;
} HW_systemClock_config_S;

/* Static Inline Functions */

/* Public Function Declarations */
bool HW_systemClock_init(const HW_systemClock_config_S * const config);

#endif // HW_SYSTEM_CLOCK_H