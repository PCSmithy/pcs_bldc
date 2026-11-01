#pragma once

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>   // size_t, NULL, offsetof

// #include "arm_math_types.h" // when added

typedef float float32_t;

typedef struct
{
    float32_t sin_rad;
    float32_t cos_rad;
} theta_S;

