/*
 * FreeRTOS application-hook stubs for native (host) unit tests.
 *
 * A unit test that links `freertos` transitively (e.g. hw_USB / io_serial pull
 * it via HW_USB_run's kernel calls) drags in tasks.c, which references
 * vApplicationIdleHook (configUSE_IDLE_HOOK) and vApplicationGetIdleTaskMemory
 * (configSUPPORT_STATIC_ALLOCATION). The firmware image gets the real ones from
 * main.c — the idle hook there is the SIL quiescence handoff — but a test never
 * links main.c and never starts the scheduler. These strong stubs satisfy the
 * link so such tests build. (Weak fallbacks were tried; MinGW/PE weak symbols in
 * a static archive don't resolve reliably, so these are plain strong defs linked
 * only into the affected test executables — never into the firmware image.)
 */
#include "FreeRTOS.h"
#include "task.h"

void vApplicationIdleHook(void)
{
}

#if (configSUPPORT_STATIC_ALLOCATION == 1)
void vApplicationGetIdleTaskMemory(StaticTask_t ** ppxTcb, StackType_t ** ppxStack, uint32_t * pulSize)
{
    static StaticTask_t xIdleTcb;
    static StackType_t  xIdleStack[configMINIMAL_STACK_SIZE];
    *ppxTcb   = &xIdleTcb;
    *ppxStack = xIdleStack;
    *pulSize  = configMINIMAL_STACK_SIZE;
}
#endif
