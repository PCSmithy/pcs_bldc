/* Includes */
#include "app_cpuLoad.h"
#include "lib_timer.h"

/* Typedefs */

typedef struct
{
    volatile uint32_t taskTime_us[APP_CPULOAD_SLOT_COUNT];   // cumulative, wraps at 2^32
    uint32_t          runningSlot;
    uint32_t          runStart_us;
} app_cpuLoad_data_S;

/* Private Data Definitions */

static app_cpuLoad_data_S app_cpuLoad_data;
static app_cpuLoad_data_S * const data = &app_cpuLoad_data;

/* Public Function Definitions */

// [impl->fw~obs_cpu_001~1]
void app_cpuLoad_switchedIn(uint32_t slot)
{
    const uint32_t now_us = lib_timer_getHWCount_us();
    const uint32_t next = (slot < APP_CPULOAD_SLOT_COUNT) ? slot : APP_CPULOAD_SLOT_OTHER;
    data->taskTime_us[data->runningSlot] += now_us - data->runStart_us;   // unsigned wrap-safe
    data->runningSlot = next;
    data->runStart_us = now_us;
}

// [impl->fw~obs_cpu_001~1]
uint32_t app_cpuLoad_getTime_us(uint32_t slot)
{
    return (slot < APP_CPULOAD_SLOT_COUNT) ? data->taskTime_us[slot] : 0U;
}
