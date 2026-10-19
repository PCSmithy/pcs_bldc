#pragma once

// Cumulative execution time per task slot, in microseconds on the
// free-running time base, accumulated at every context switch. The kernel's
// switched-in hook calls app_cpuLoad_switchedIn with the incoming task's
// number; a task with no number lands in slot 0. Interrupt time is charged to
// the task it interrupts; the cycle interrupt keeps its own total in HW_ADC.

/* Includes */
#include "lib_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Defines */

#define APP_CPULOAD_SLOT_COUNT (8U)
#define APP_CPULOAD_SLOT_OTHER (0U)   // tasks without a number, and any number past the slots

/* Public Function Declarations */

// The scheduler just switched to the task holding this slot: close the
// previous slot's run and open this one. Any context, any time base state.
void app_cpuLoad_switchedIn(uint32_t slot);

// A slot's cumulative microseconds (modulo 2^32); zero for a slot past the
// count.
uint32_t app_cpuLoad_getTime_us(uint32_t slot);

#ifdef __cplusplus
}
#endif
