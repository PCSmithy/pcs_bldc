#ifndef MOCK_TASK_H
#define MOCK_TASK_H

// Minimal mock of the FreeRTOS task API used by app_rgbLedRing.c. Implemented as
// no-op stubs in mock_app_deps.c.
#include "FreeRTOS.h"

typedef void (*TaskFunction_t)(void *);

BaseType_t xTaskCreate(TaskFunction_t taskFn, const char * name, uint32_t stackDepth,
                       void * params, UBaseType_t priority, TaskHandle_t * handle);
TickType_t xTaskGetTickCount(void);
void       vTaskDelay(TickType_t ticks);
void       vTaskDelayUntil(TickType_t * prevWake, TickType_t increment);
void       vTaskSuspendAll(void);
BaseType_t xTaskResumeAll(void);

#endif // MOCK_TASK_H
