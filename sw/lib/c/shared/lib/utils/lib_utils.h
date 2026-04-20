#ifndef LIB_UTILS_H
#define LIB_UTILS_H

// Cross-cutting utility macros. Add new ones here as concrete needs
// arise; resist the urge to pre-stock with "everyone usually has
// MIN/MAX/CLAMP" until something actually needs them.


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


#endif /* LIB_UTILS_H */
