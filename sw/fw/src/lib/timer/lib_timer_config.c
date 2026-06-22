
#include "lib_types.h"

#include "lib_timer.h"

#include "HW_TIM.h"


static uint32_t lib_timer_config_private_getTime_us(void)
{
    uint32_t cnt = 0U;
    HW_TIM_getCounter(HW_TIM_CHANNEL_2, &cnt);
    return cnt;
}

const lib_timer_config_S lib_timer_config =
{
    .getTime_us = lib_timer_config_private_getTime_us,
};
