
#include "lib_types.h"

#include "lib_timer.h"

#include "HW_TIM.h"

#include "FreeRTOS.h"
#include "task.h"


// taskENTER_CRITICAL/taskEXIT_CRITICAL are macros, so the hooks need real
// functions to point at. Every lib_timer caller runs in a task.
static void lib_timer_config_private_enterCritical(void)
{
    taskENTER_CRITICAL();
}

static void lib_timer_config_private_exitCritical(void)
{
    taskEXIT_CRITICAL();
}

static uint32_t lib_timer_config_private_getTime_us(void)
{
    uint32_t cnt = 0U;
    HW_TIM_getCounter(HW_TIM_PERIPHERAL_2, &cnt);
    return cnt;
}

const lib_timer_config_S lib_timer_config =
{
    .getTime_us    = lib_timer_config_private_getTime_us,
    .enterCritical = lib_timer_config_private_enterCritical,
    .exitCritical  = lib_timer_config_private_exitCritical,
};
