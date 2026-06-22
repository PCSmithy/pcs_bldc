
/* Includes */
#include "lib_timer.h"

extern const lib_timer_config_S lib_timer_config;

/* Defines */

/* Typedefs */
typedef struct
{
    const lib_timer_config_S * config;

    uint32_t lastTime_u32;
    uint64_t currentTime_us;
} lib_timer_data_S;

/* Private Function Declarations */
static void lib_timer_private_update(void);

/* Private Data Definitions */
static lib_timer_data_S lib_timer_data = { .config = &lib_timer_config };
static lib_timer_data_S * const data = &lib_timer_data;

/* Private Function Definitions */

static void lib_timer_private_update(void)
{
    if (data->config->getTime_us != NULL)
    {
        const uint32_t time_u32 = data->config->getTime_us();

        // Accumulate the elapsed delta. Unsigned subtraction wraps modulo
        // 2^32, so a HW-counter rollover (time_u32 < lastTime_u32) yields the
        // correct delta with no special case.
        const uint32_t delta_us = time_u32 - data->lastTime_u32;
        data->currentTime_us += delta_us;
        data->lastTime_u32 = time_u32;
    }
}

/* Public Function Definitions */
// void lib_timer_init(lib_timer_channel_S * timer, lib_timer_precision_E precision, uint64_t duration)

void lib_timer_run(void)
{
    lib_timer_private_update();
}

uint64_t lib_timer_getTime_us(void)
{
    lib_timer_private_update();
    return data->currentTime_us;
}

uint32_t lib_timer_getTime_ms(void)
{
    lib_timer_private_update();
    return data->currentTime_us / 1000U;
}
