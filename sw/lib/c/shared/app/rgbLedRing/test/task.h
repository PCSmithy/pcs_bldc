#ifndef MOCK_TASK_H
#define MOCK_TASK_H

// Minimal mock of the FreeRTOS task API used by app_rgbLedRing.c: scheduler
// suspend/resume around the SK6805 transmit. No-op stubs in mock_app_deps.c.
#include "FreeRTOS.h"

void       vTaskSuspendAll(void);
BaseType_t xTaskResumeAll(void);

#endif // MOCK_TASK_H
