
#include "lib_types.h"
#include "lib_build.h"

#include "lib_timer.h"

#include "HW_TIM.h"
#if (BUILD_TARGET == BUILD_TARGET_STM32G4)
#include "stm32g4xx_hal.h"
#endif

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

// Read on every 20 kHz cycle, several times: on the target the counter
// register is read directly, sparing the validated HW_TIM path; the sim
// keeps that path so SIL time flows from the sim timer.
static uint32_t lib_timer_config_private_getTime_us(void)
{
#if (BUILD_TARGET == BUILD_TARGET_STM32G4)
    return TIM2->CNT;
#else
    uint32_t cnt = 0U;
    HW_TIM_getCounter(HW_TIM_PERIPHERAL_2, &cnt);
    return cnt;
#endif
}

const lib_timer_config_S lib_timer_config =
{
    .getTime_us    = lib_timer_config_private_getTime_us,
    .enterCritical = lib_timer_config_private_enterCritical,
    .exitCritical  = lib_timer_config_private_exitCritical,
};
