#ifndef MOCK_FREERTOS_H
#define MOCK_FREERTOS_H

// Minimal mock of the FreeRTOS surface app_rgbLedRing.c uses (the SK6805 stream
// guard). Lets the merged TU compile in the unit test without the real kernel;
// the run10ms code links against mock_app_deps.c but is never run by the tests.
#include "lib_types.h"

typedef int32_t BaseType_t;

#define pdPASS ((BaseType_t)1)

#endif // MOCK_FREERTOS_H
