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

    // Per-pin injected input level and EXTI registration for SIL tests.
    HW_GPIO_level_E        inputLevel[HW_GPIO_PORT_COUNT][HW_GPIO_SIM_PINS_PER_PORT];
    HW_GPIO_extiCallback_F extiCallback[HW_GPIO_PORT_COUNT][HW_GPIO_SIM_PINS_PER_PORT];
    void *                 extiContext[HW_GPIO_PORT_COUNT][HW_GPIO_SIM_PINS_PER_PORT];

    // Polled-input cache (mirror of the stm32g4 driver): inputMask marks the
    // configured GPIO_MODE_INPUT pins, cachedInput holds their last sample.
    uint16_t inputMask[HW_GPIO_PORT_COUNT];
    uint16_t cachedInput[HW_GPIO_PORT_COUNT];
} HW_GPIO_data_S;

/* Private Function Declarations */

/* Private Data Definitions */

static HW_GPIO_data_S HW_GPIO_data;
static HW_GPIO_data_S * const data = &HW_GPIO_data;

/* Private Function Definitions */

// [impl->fw~hal_gpio_002~1]
static bool HW_GPIO_private_pinConfigValid(const HW_GPIO_pinConfig_S * const pinConfig)
{
    const bool pinValid  = ((pinConfig->pin != 0U) && ((pinConfig->pin & ~0xFFFFUL) == 0U));
    const bool modeValid = (pinConfig->mode <= HW_GPIO_MODE_INTERRUPT);
    return ((pinValid) && (modeValid));
}

/* Public Function Definitions */
// [impl->fw~hal_gpio_001~1]
bool HW_GPIO_init(const HW_GPIO_config_S * const config)
{
    bool ret = false;
    if (config != NULL)
    {
        // Validate every declared pin before applying anything so a bad
        // config fails init cleanly rather than half-configuring.
        bool allValid = true;
        for (HW_GPIO_port_E port = 0U; port < HW_GPIO_PORT_COUNT; port++)
        {
            const HW_GPIO_portConfig_S * const portConfig = &config->ports[port];
            for (size_t pin = 0U; pin < portConfig->numPins; pin++)
            {
                if (!HW_GPIO_private_pinConfigValid(&portConfig->pins[pin]))
                {
                    allValid = false;
                }
            }
        }

        if (allValid)
        {
            // Record input pins so HW_GPIO_run1ms() knows what to poll.
            for (HW_GPIO_port_E port = 0U; port < HW_GPIO_PORT_COUNT; port++)
            {
                const HW_GPIO_portConfig_S * const portConfig = &config->ports[port];
                for (size_t pin = 0U; pin < portConfig->numPins; pin++)
                {
                    if (portConfig->pins[pin].mode == HW_GPIO_MODE_INPUT)
                    {
                        data->inputMask[port] |= (uint16_t)portConfig->pins[pin].pin;
                    }
                }
            }

            data->config      = config;
            data->initialized = true;
            ret = true;
        }
    }
    return ret;
}

// [impl->fw~hal_gpio_003~1]
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

// [impl->fw~hal_gpio_004~1]
HW_GPIO_level_E HW_GPIO_readPin(HW_GPIO_port_E port, uint32_t pin)
{
    HW_GPIO_level_E level = HW_GPIO_LEVEL_LOW;
    if (port < HW_GPIO_PORT_COUNT)
    {
        for (uint32_t bit = 0U; bit < HW_GPIO_SIM_PINS_PER_PORT; bit++)
        {
            if ((pin & (1UL << bit)) != 0U)
            {
                level = data->inputLevel[port][bit];
                break;
            }
        }
    }
    return level;
}

void HW_GPIO_run1ms(void)
{
    for (HW_GPIO_port_E port = 0U; port < HW_GPIO_PORT_COUNT; port++)
    {
        uint16_t snapshot = 0U;
        for (uint32_t bit = 0U; bit < HW_GPIO_SIM_PINS_PER_PORT; bit++)
        {
            if (((data->inputMask[port] & (uint16_t)(1UL << bit)) != 0U) &&
                (data->inputLevel[port][bit] == HW_GPIO_LEVEL_HIGH))
            {
                snapshot |= (uint16_t)(1UL << bit);
            }
        }
        data->cachedInput[port] = snapshot;
    }
}

HW_GPIO_level_E HW_GPIO_readCached(HW_GPIO_port_E port, uint32_t pin)
{
    HW_GPIO_level_E ret = HW_GPIO_LEVEL_LOW;
    if (port < HW_GPIO_PORT_COUNT)
    {
        if ((data->cachedInput[port] & (uint16_t)pin) != 0U)
        {
            ret = HW_GPIO_LEVEL_HIGH;
        }
    }
    return ret;
}

// [impl->fw~hal_gpio_005~1]
bool HW_GPIO_registerExtiCallback(HW_GPIO_port_E port, uint32_t pin, HW_GPIO_extiCallback_F callback, void * context)
{
    bool ret = false;
    if (port < HW_GPIO_PORT_COUNT)
    {
        for (uint32_t bit = 0U; bit < HW_GPIO_SIM_PINS_PER_PORT; bit++)
        {
            if ((pin & (1UL << bit)) != 0U)
            {
                data->extiCallback[port][bit] = callback;
                data->extiContext[port][bit]  = context;
            }
        }
        ret = true;
    }
    return ret;
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
            data->level[port][bit]        = HW_GPIO_LEVEL_LOW;
            data->writeCount[port][bit]   = 0U;
            data->inputLevel[port][bit]   = HW_GPIO_LEVEL_LOW;
            data->extiCallback[port][bit] = NULL;
            data->extiContext[port][bit]  = NULL;
        }
        data->inputMask[port]   = 0U;
        data->cachedInput[port] = 0U;
    }
}

void HW_GPIO_sim_setInputLevel(HW_GPIO_port_E port, uint32_t pin, HW_GPIO_level_E level)
{
    if (port < HW_GPIO_PORT_COUNT)
    {
        for (uint32_t bit = 0U; bit < HW_GPIO_SIM_PINS_PER_PORT; bit++)
        {
            if ((pin & (1UL << bit)) != 0U)
            {
                data->inputLevel[port][bit] = level;
            }
        }
    }
}

void HW_GPIO_sim_triggerExti(HW_GPIO_port_E port, uint32_t pin)
{
    if (port < HW_GPIO_PORT_COUNT)
    {
        for (uint32_t bit = 0U; bit < HW_GPIO_SIM_PINS_PER_PORT; bit++)
        {
            if ((pin & (1UL << bit)) != 0U)
            {
                if (data->extiCallback[port][bit] != NULL)
                {
                    data->extiCallback[port][bit](port, (1UL << bit), data->extiContext[port][bit]);
                }
            }
        }
    }
}
