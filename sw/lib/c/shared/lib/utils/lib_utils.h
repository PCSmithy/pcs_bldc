#pragma once

#include "lib_types.h"

#define PI (3.14159265359f)

#define DEG_TO_RAD(a) ((a) * PI / 180.0f)
#define RPM_TO_RAD_PER_SEC(a) ((a) * 2.0f * PI / 60.0f)

// Wrap a radian angle one step into (-PI, PI]. Single-step: correct for
// inputs within (-2*PI, 2*PI), e.g. the difference of two angles each in
// [0, 2*PI).
#define WRAP_RAD_TO_PI(a) \
    (((a) > PI) ? ((a) - (2.0f * PI)) : ((((a) < -PI)) ? ((a) + (2.0f * PI)) : (a)))

/**
 * COUNTOF(arr) — number of elements in a static-sized array.
 *
 * Compile-time error if `arr` is a pointer (e.g. an array that decayed
 * when passed to a function), preventing the silent wrong-result bug
 * that the bare `sizeof(arr) / sizeof(arr[0])` idiom has.
 *
 * Uses GCC builtins (__builtin_types_compatible_p, __typeof__). Both the
 * native (MinGW gcc 15.2) and embedded (arm-none-eabi-gcc 15.2)
 * toolchains support them, and the underscore-prefixed __typeof__ form
 * works even under -std=c11 -Wpedantic. If a non-GCC compiler is ever
 * introduced, this macro will need a fallback.
 */
#define COUNTOF(arr) \
    (sizeof(arr) / sizeof((arr)[0]) + \
     sizeof(__typeof__(int[1 - 2 * !!__builtin_types_compatible_p( \
         __typeof__(arr), __typeof__(&(arr)[0]))])) * 0)




static inline void floatToFixed(float32_t value, uint32_t scale,
                                uint32_t * whole, uint32_t * frac)
{
    const uint32_t scaled = (uint32_t)(value * (float32_t)scale + 0.5f);
    *whole = scaled / scale;
    *frac  = scaled % scale;
}

#define US_TO_MS(us) ((us) / 1000)

#define MIN_OF(a, b) ((a) < (b) ? (a) : (b))
#define MAX_OF(a, b) ((a) > (b) ? (a) : (b))

#define SIGN(a) ((a) > 0U ? (1) : (-1))