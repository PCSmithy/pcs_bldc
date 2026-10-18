#pragma once

// Target-specific half of HW_CORDIC; reached via HW_CORDIC.h.

/* Includes */
#include "lib_types.h"

/* Typedefs */

typedef struct
{
    // Iterations in units of four, 1..15: each unit costs one clock and
    // gains about one decimal digit; 6 is 24 iterations for a 24-bit float.
    uint8_t precisionCycles;
} HW_CORDIC_config_S;
