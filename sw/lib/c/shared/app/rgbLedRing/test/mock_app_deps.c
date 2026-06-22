// No-op stubs for the FreeRTOS + IO/dev symbols app_rgbLedRing.c's task/init/
// blink code references. The render-core tests never call that code, but it is
// compiled into the test executable and must link.
#include "FreeRTOS.h"
#include "task.h"
#include "IO_SK6805.h"
#include "IO_AS5048.h"
#include "DEV_switch.h"

BaseType_t xTaskCreate(TaskFunction_t taskFn, const char * name, uint32_t stackDepth,
                       void * params, UBaseType_t priority, TaskHandle_t * handle)
{
    (void)taskFn; (void)name; (void)stackDepth; (void)params; (void)priority; (void)handle;
    return pdPASS;
}

TickType_t xTaskGetTickCount(void)                       { return 0U; }
void       vTaskDelay(TickType_t ticks)                  { (void)ticks; }
void       vTaskDelayUntil(TickType_t * p, TickType_t i) { (void)p; (void)i; }
void       vTaskSuspendAll(void)                         {}
BaseType_t xTaskResumeAll(void)                          { return pdPASS; }

void IO_SK6805_setPixel(IO_SK6805_channel_E channel, uint16_t index, uint8_t red, uint8_t green, uint8_t blue)
{
    (void)channel; (void)index; (void)red; (void)green; (void)blue;
}
void IO_SK6805_setAll(IO_SK6805_channel_E channel, uint8_t red, uint8_t green, uint8_t blue)
{
    (void)channel; (void)red; (void)green; (void)blue;
}
void IO_SK6805_clear(IO_SK6805_channel_E channel)        { (void)channel; }
bool IO_SK6805_update(IO_SK6805_channel_E channel)       { (void)channel; return true; }

bool IO_AS5048_readAngle(IO_AS5048_channel_E channel, uint16_t * angleRaw, float32_t * angle_deg)
{
    (void)channel;
    if (angleRaw != NULL)  { *angleRaw = 0U; }
    if (angle_deg != NULL) { *angle_deg = 0.0f; }
    return true;
}

bool DEV_switch_isActive(DEV_switch_channel_E channel)   { (void)channel; return false; }
