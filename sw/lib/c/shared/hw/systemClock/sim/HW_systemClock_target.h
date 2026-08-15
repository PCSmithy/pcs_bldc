#pragma once

// Target-specific half of HW_systemClock; reached via HW_systemClock.h.

/* Includes */
#include "lib_types.h"

/* Typedefs */

// The sim has no clock tree to program, so there is nothing to configure.
typedef struct
{
    uint8_t _void;
} HW_systemClock_config_S;
