#ifndef MOCK_FREERTOS_H
#define MOCK_FREERTOS_H

// Minimal mock of FreeRTOS core types/macros — only the surface app_rgbLedRing.c
// uses. Lets the merged TU compile in the unit test without the real kernel; the
// task/init code links against mock_app_deps.c but is never run by the tests.
#include "lib_types.h"

typedef uint32_t TickType_t;
typedef uint32_t UBaseType_t;
typedef int32_t  BaseType_t;
typedef void *   TaskHandle_t;

#define pdPASS                     ((BaseType_t)1)
#define pdMS_TO_TICKS(ms)          ((TickType_t)(ms))
#define configMINIMAL_STACK_SIZE   128U

#endif // MOCK_FREERTOS_H
