/* Includes */
#include "HW_GPIO.h"
#include "HW_GPIO_simData.h"
#include "SIL_ports.h"

/* Private Function Declarations */

static bool HW_GPIO_private_pinConfigValid(const HW_GPIO_pinConfig_S * const pinConfig);
static size_t HW_GPIO_private_numPins(const HW_GPIO_portConfig_S * const portConfig);
static void HW_GPIO_private_publishOutputs(HW_GPIO_port_E port, uint32_t pin, HW_GPIO_level_E level);

/* Private Data Definitions */

HW_GPIO_data_S HW_GPIO_data;
static HW_GPIO_data_S * const data = &HW_GPIO_data;

/* Private Function Definitions */

// [impl->fw~hal_gpio_002~1]
static bool HW_GPIO_private_pinConfigValid(const HW_GPIO_pinConfig_S * const pinConfig)
{
    const bool pinValid  = ((pinConfig->pin != 0U) && ((pinConfig->pin & ~0xFFFFUL) == 0U));
    const bool modeValid = (pinConfig->mode <= HW_GPIO_MODE_INTERRUPT_BOTH);
    return ((pinValid) && (modeValid));
}

// Declared pin count, clamped to the one-handle-per-config-entry table: a port
// has at most 16 lines, so a longer array cannot describe distinct pins.
static size_t HW_GPIO_private_numPins(const HW_GPIO_portConfig_S * const portConfig)
{
    return ((portConfig->numPins < HW_GPIO_SIM_PINS_PER_PORT)
        ? portConfig->numPins : HW_GPIO_SIM_PINS_PER_PORT);
}

// Publish the driven level on every configured output pin the write touched: a
// pin mask may carry several lines, and any overlap publishes that entry's port.
static void HW_GPIO_private_publishOutputs(HW_GPIO_port_E port, uint32_t pin, HW_GPIO_level_E level)
{
    const HW_GPIO_portConfig_S * const portConfig = &data->config->ports[port];
    const size_t numPins = HW_GPIO_private_numPins(portConfig);
    for (size_t i = 0U; i < numPins; i++)
    {
        if ((portConfig->pins[i].pin & pin) != 0U)
        {
            SIL_ports_write(data->outputHandle[port][i], (level == HW_GPIO_LEVEL_HIGH) ? 1.0 : 0.0);
        }
    }
}

/* Public Function Definitions */
// [impl->fw~hal_gpio_001~1]
bool HW_GPIO_init(const HW_GPIO_config_S * const config)
{
    bool ret = false;

    // Re-entrant: every call, accepted or rejected, is a clean slate.
    *data = (HW_GPIO_data_S){ 0 };
    for (HW_GPIO_port_E port = 0U; port < HW_GPIO_PORT_COUNT; port++)
    {
        for (uint32_t bit = 0U; bit < HW_GPIO_SIM_PINS_PER_PORT; bit++)
        {
            data->outputHandle[port][bit] = SIL_PORTS_HANDLE_INVALID;
        }
    }

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
            // Record what HW_GPIO_run1ms() polls and edge-detects, and register
            // one observation port per named output pin.
            for (HW_GPIO_port_E port = 0U; port < HW_GPIO_PORT_COUNT; port++)
            {
                const HW_GPIO_portConfig_S * const portConfig = &config->ports[port];
                const size_t numPins = HW_GPIO_private_numPins(portConfig);
                for (size_t pin = 0U; pin < numPins; pin++)
                {
                    if (portConfig->pins[pin].mode == HW_GPIO_MODE_INPUT)
                    {
                        data->inputMask[port] |= (uint16_t)portConfig->pins[pin].pin;
                    }
                    else if (portConfig->pins[pin].mode >= HW_GPIO_MODE_INTERRUPT_RISING)
                    {
                        const HW_GPIO_mode_E mode = portConfig->pins[pin].mode;
                        const uint16_t mask = (uint16_t)portConfig->pins[pin].pin;
                        data->interruptMask[port] |= mask;
                        if ((mode == HW_GPIO_MODE_INTERRUPT_RISING) || (mode == HW_GPIO_MODE_INTERRUPT_BOTH))
                        {
                            data->risingMask[port] |= mask;
                        }
                        if ((mode == HW_GPIO_MODE_INTERRUPT_FALLING) || (mode == HW_GPIO_MODE_INTERRUPT_BOTH))
                        {
                            data->fallingMask[port] |= mask;
                        }
                    }
                    else if ((portConfig->pins[pin].mode == HW_GPIO_MODE_OUTPUT) &&
                             (portConfig->pins[pin].pinNameStr != NULL))
                    {
                        data->outputHandle[port][pin] =
                            SIL_ports_register("vsig", portConfig->pins[pin].pinNameStr, NULL);
                    }
                    else
                    {
                        // Unnamed output pins register nothing.
                    }
                }
            }

            data->config      = config;
            data->initialized = true;

            // Boot state: every output pin reads low until firmware drives it.
            for (HW_GPIO_port_E port = 0U; port < HW_GPIO_PORT_COUNT; port++)
            {
                const size_t numPins = HW_GPIO_private_numPins(&config->ports[port]);
                for (size_t pin = 0U; pin < numPins; pin++)
                {
                    SIL_ports_write(data->outputHandle[port][pin], 0.0);
                }
            }
            ret = true;
        }
    }
    return ret;
}

// [impl->fw~hal_gpio_003~1]
void HW_GPIO_writePin(HW_GPIO_port_E port, uint32_t pin, HW_GPIO_level_E level)
{
    if ((data->initialized) && (port < HW_GPIO_PORT_COUNT))
    {
        HW_GPIO_private_publishOutputs(port, pin, level);
    }
}

// [impl->fw~hal_gpio_004~1]
// [impl->fw~hal_gpio_005~1]
void HW_GPIO_run1ms(void)
{
    if (data->initialized)
    {
        for (HW_GPIO_port_E port = 0U; port < HW_GPIO_PORT_COUNT; port++)
        {
            uint16_t levels = 0U;
            for (uint32_t bit = 0U; bit < HW_GPIO_SIM_PINS_PER_PORT; bit++)
            {
                if (data->inputLevel[port][bit] == HW_GPIO_LEVEL_HIGH)
                {
                    levels |= (uint16_t)(1UL << bit);
                }
            }
            data->cachedInput[port] = (uint16_t)(levels & data->inputMask[port]);

            // The injected level's transitions are the EXTI line's sim twin, kept
            // to the edge each pin's trigger type accepts. Sampled, not clocked:
            // transitions inside one pass are only visible in the net level, so an
            // even number of them between passes registers as no edge at all.
            const uint16_t irqLevels = (uint16_t)(levels & data->interruptMask[port]);
            const uint16_t changed   = (uint16_t)(irqLevels ^ data->lastInterruptLevel[port]);
            const uint16_t accepted  = (uint16_t)((data->risingMask[port] & irqLevels) |
                                                  (data->fallingMask[port] & (uint16_t)~irqLevels));
            const uint16_t edges     = (uint16_t)(changed & accepted);
            data->lastInterruptLevel[port] = irqLevels;
            for (uint32_t bit = 0U; bit < HW_GPIO_SIM_PINS_PER_PORT; bit++)
            {
                if ((edges & (uint16_t)(1UL << bit)) != 0U)
                {
                    data->extiEdgeCount[port]++;
                    if (data->extiCallback[port][bit] != NULL)
                    {
                        data->extiCallback[port][bit](port, (1UL << bit), data->extiContext[port][bit]);
                    }
                }
            }
        }
    }
}

// [impl->fw~hal_gpio_004~1]
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
