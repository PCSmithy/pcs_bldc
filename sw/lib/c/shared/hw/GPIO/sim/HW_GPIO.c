/* Includes */
#include "HW_GPIO.h"
#include "HW_GPIO_sim.h"

/* Defines */

#define HW_GPIO_SIM_PINS_PER_PORT    (16U)

/* Typedefs */
typedef struct
{
    const HW_GPIO_config_S * config;
    bool initialized;

    // Per-pin recorded write state for SIL inspection. Indexed by port
    // then by bit position (0..15) within the pin mask.
    HW_GPIO_level_E level[HW_GPIO_PORT_COUNT][HW_GPIO_SIM_PINS_PER_PORT];
    uint32_t        writeCount[HW_GPIO_PORT_COUNT][HW_GPIO_SIM_PINS_PER_PORT];
} HW_GPIO_data_S;

/* Private Function Declarations */

/* Private Data Definitions */

static HW_GPIO_data_S HW_GPIO_data;
static HW_GPIO_data_S * const data = &HW_GPIO_data;

/* Private Function Definitions */

/* Public Function Definitions */
bool HW_GPIO_init(const HW_GPIO_config_S * const config)
{
    bool ret = false;
    if (config != NULL)
    {
        // Sim has no real pins to configure — accept the config,
        // remember it (future SIL tests may want to query pin states),
        // and report success. Project policy: init returns bool so
        // main.c handles failure uniformly across targets.
        data->config      = config;
        data->initialized = true;
        ret = true;
    }
    return ret;
}

void HW_GPIO_writePin(HW_GPIO_port_E port, uint32_t pin, HW_GPIO_level_E level)
{
    if (port < HW_GPIO_PORT_COUNT)
    {
        // A pin mask may carry more than one bit; record each so tests
        // can inspect any pin the driver drove.
        for (uint32_t bit = 0U; bit < HW_GPIO_SIM_PINS_PER_PORT; bit++)
        {
            if ((pin & (1UL << bit)) != 0U)
            {
                data->level[port][bit] = level;
                data->writeCount[port][bit]++;
            }
        }
    }
}

HW_GPIO_level_E HW_GPIO_sim_getLevel(HW_GPIO_port_E port, uint32_t pin)
{
    HW_GPIO_level_E level = HW_GPIO_LEVEL_LOW;
    if (port < HW_GPIO_PORT_COUNT)
    {
        for (uint32_t bit = 0U; bit < HW_GPIO_SIM_PINS_PER_PORT; bit++)
        {
            if ((pin & (1UL << bit)) != 0U)
            {
                level = data->level[port][bit];
                break;
            }
        }
    }
    return level;
}

uint32_t HW_GPIO_sim_getWriteCount(HW_GPIO_port_E port, uint32_t pin)
{
    uint32_t count = 0U;
    if (port < HW_GPIO_PORT_COUNT)
    {
        for (uint32_t bit = 0U; bit < HW_GPIO_SIM_PINS_PER_PORT; bit++)
        {
            if ((pin & (1UL << bit)) != 0U)
            {
                count = data->writeCount[port][bit];
                break;
            }
        }
    }
    return count;
}

void HW_GPIO_sim_reset(void)
{
    for (HW_GPIO_port_E port = 0U; port < HW_GPIO_PORT_COUNT; port++)
    {
        for (uint32_t bit = 0U; bit < HW_GPIO_SIM_PINS_PER_PORT; bit++)
        {
            data->level[port][bit]      = HW_GPIO_LEVEL_LOW;
            data->writeCount[port][bit] = 0U;
        }
    }
}
