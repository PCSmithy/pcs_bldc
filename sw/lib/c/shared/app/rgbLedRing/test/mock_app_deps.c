// No-op stubs for the FreeRTOS + IO/dev symbols app_rgbLedRing.c's run10ms code
// references. The render-core tests never call run10ms, but it is compiled into
// the test executable and must link.
#include "FreeRTOS.h"
#include "task.h"
#include "IO_SK6805.h"
#include "IO_AS5048.h"
#include "dev_switch.h"

void       vTaskSuspendAll(void) {}
BaseType_t xTaskResumeAll(void) { return pdPASS; }

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

bool dev_switch_isActive(dev_switch_channel_E channel)   { (void)channel; return false; }
