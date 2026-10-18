#pragma once

// Target-specific half of HW_CORDIC; reached via HW_CORDIC.h.

/* Includes */
#include "lib_types.h"

/* Typedefs */

// The sim computes with libm, so there is nothing to configure.
typedef struct
{
    uint8_t _void;
} HW_CORDIC_config_S;
