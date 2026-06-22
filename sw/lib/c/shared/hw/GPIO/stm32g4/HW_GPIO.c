/* Includes */
#include "HW_GPIO.h"
#include "stm32g4xx_hal.h"

/* Defines */

// Preemption priority for GPIO EXTI lines. Kept numerically high enough to
// sit below the FreeRTOS syscall ceiling, so the ISR stays RTOS-safe once
// FreeRTOS is wired up.
#define HW_GPIO_EXTI_IRQ_PRIORITY    (5U)

/* Typedefs */
// One entry per EXTI line (0..15). HAL_GPIO_EXTI_Callback dispatches the
// pending line here, so registration and dispatch share this table.
typedef struct
{
    HW_GPIO_port_E port;
    HW_GPIO_extiCallback_F callback;
    void * context;
} HW_GPIO_extiEntry_S;

typedef struct
{
    const HW_GPIO_config_S * config;
    HW_GPIO_extiEntry_S extiTable[16U];

    // Polled-input cache. inputMask[port] has a 1 for each pin configured as
    // GPIO_MODE_INPUT; cachedInput[port] holds their last-sampled levels.
    uint16_t inputMask[HW_GPIO_PORT_COUNT];
    uint16_t cachedInput[HW_GPIO_PORT_COUNT];
} HW_GPIO_data_S;

/* Private Function Declarations */

static void HW_GPIO_private_enablePortClock(HW_GPIO_port_E port);
static bool HW_GPIO_private_pinConfigValid(const GPIO_InitTypeDef * const pinConfig);
static void HW_GPIO_private_enableExtiNvic(uint32_t pin);

/* Private Data Definitions */

static HW_GPIO_data_S HW_GPIO_data;
static HW_GPIO_data_S * const data = &HW_GPIO_data;

// Map our port enum to the HAL's GPIO_TypeDef* peripheral handles.
static GPIO_TypeDef * const HW_GPIO_portHandleMapping[HW_GPIO_PORT_COUNT] =
{
    [HW_GPIO_PORT_A] = GPIOA,
    [HW_GPIO_PORT_B] = GPIOB,
    [HW_GPIO_PORT_C] = GPIOC,
    [HW_GPIO_PORT_D] = GPIOD,
    [HW_GPIO_PORT_E] = GPIOE,
    [HW_GPIO_PORT_F] = GPIOF,
    [HW_GPIO_PORT_G] = GPIOG,
};

/* Private Function Definitions */

// __HAL_RCC_GPIOx_CLK_ENABLE() are macros that expand to specific
// register writes per port; they can't be looked up via the
// portHandleMapping table. Switch is the cleanest dispatch.
static void HW_GPIO_private_enablePortClock(HW_GPIO_port_E port)
{
    switch (port)
    {
        case HW_GPIO_PORT_A: __HAL_RCC_GPIOA_CLK_ENABLE(); break;
        case HW_GPIO_PORT_B: __HAL_RCC_GPIOB_CLK_ENABLE(); break;
        case HW_GPIO_PORT_C: __HAL_RCC_GPIOC_CLK_ENABLE(); break;
        case HW_GPIO_PORT_D: __HAL_RCC_GPIOD_CLK_ENABLE(); break;
        case HW_GPIO_PORT_E: __HAL_RCC_GPIOE_CLK_ENABLE(); break;
        case HW_GPIO_PORT_F: __HAL_RCC_GPIOF_CLK_ENABLE(); break;
        case HW_GPIO_PORT_G: __HAL_RCC_GPIOG_CLK_ENABLE(); break;
        default:                                           break;
    }
}

// [impl->fw~hal_gpio_002~1]
static bool HW_GPIO_private_pinConfigValid(const GPIO_InitTypeDef * const pinConfig)
{
    // Non-empty selection (at least one line) with no bits above line 15;
    // multi-bit masks are allowed.
    const bool pinValid = ((pinConfig->Pin != 0U) && ((pinConfig->Pin & ~0xFFFFUL) == 0U));

    const bool modeValid = ((pinConfig->Mode == GPIO_MODE_INPUT) ||
                            (pinConfig->Mode == GPIO_MODE_OUTPUT_PP) ||
                            (pinConfig->Mode == GPIO_MODE_OUTPUT_OD) ||
                            (pinConfig->Mode == GPIO_MODE_IT_RISING) ||
                            (pinConfig->Mode == GPIO_MODE_IT_FALLING) ||
                            (pinConfig->Mode == GPIO_MODE_IT_RISING_FALLING));

    return ((pinValid) && (modeValid));
}

// Unmask the NVIC EXTI interrupt(s) for the line(s) in `pin` so a configured
// interrupt pin actually reaches HAL_GPIO_EXTI_IRQHandler. On STM32G4 lines
// 0..4 have dedicated IRQs, lines 5..9 share EXTI9_5, and 10..15 share
// EXTI15_10.
// [impl->fw~hal_gpio_005~1]
static void HW_GPIO_private_enableExtiNvic(uint32_t pin)
{
    for (uint32_t bit = 0U; bit < 16U; bit++)
    {
        if ((pin & (1UL << bit)) != 0U)
        {
            const IRQn_Type irq = (bit <= 4U) ? (IRQn_Type)((uint32_t)EXTI0_IRQn + bit) :
                                  (bit <= 9U) ? EXTI9_5_IRQn :
                                                EXTI15_10_IRQn;
            HAL_NVIC_SetPriority(irq, HW_GPIO_EXTI_IRQ_PRIORITY, 0U);
            HAL_NVIC_EnableIRQ(irq);
        }
    }
}

/* Public Function Definitions */
// [impl->fw~hal_gpio_001~1]
bool HW_GPIO_init(const HW_GPIO_config_S * const config)
{
    bool ret = false;
    if (config != NULL)
    {
        // Validate every declared pin before touching hardware so a bad
        // config fails init cleanly rather than half-configuring ports.
        bool allValid = true;
        for (HW_GPIO_port_E port = 0U; ((port < HW_GPIO_PORT_COUNT) && (allValid)); port++)
        {
            const HW_GPIO_portConfig_S * const portConfig = &config->ports[port];
            for (size_t i = 0U; ((i < portConfig->numPins) && (allValid)); i++)
            {
                if (!HW_GPIO_private_pinConfigValid(&portConfig->pins[i]))
                {
                    allValid = false;
                }
            }
        }

        if (allValid)
        {
            for (HW_GPIO_port_E port = 0U; port < HW_GPIO_PORT_COUNT; port++)
            {
                const HW_GPIO_portConfig_S * const portConfig = &config->ports[port];
                if (portConfig->numPins == 0U)
                {
                    continue;
                }

                HW_GPIO_private_enablePortClock(port);

                for (size_t i = 0U; i < portConfig->numPins; i++)
                {
                    // HAL takes non-const but doesn't mutate; const-cast.
                    GPIO_InitTypeDef * const pinConfig = (GPIO_InitTypeDef *)&portConfig->pins[i];

                    // For output pins, set the initial level BEFORE init so
                    // the pin doesn't glitch when switched out of reset state.
                    if ((pinConfig->Mode == GPIO_MODE_OUTPUT_PP) ||
                        (pinConfig->Mode == GPIO_MODE_OUTPUT_OD))
                    {
                        HAL_GPIO_WritePin(HW_GPIO_portHandleMapping[port], pinConfig->Pin, GPIO_PIN_RESET);
                    }

                    HAL_GPIO_Init(HW_GPIO_portHandleMapping[port], pinConfig);

                    // Interrupt-mode pins also need their EXTI line unmasked
                    // in the NVIC to actually fire.
                    if ((pinConfig->Mode == GPIO_MODE_IT_RISING) ||
                        (pinConfig->Mode == GPIO_MODE_IT_FALLING) ||
                        (pinConfig->Mode == GPIO_MODE_IT_RISING_FALLING))
                    {
                        HW_GPIO_private_enableExtiNvic(pinConfig->Pin);
                    }

                    // Record input pins so HW_GPIO_run1ms() knows what to poll.
                    if (pinConfig->Mode == GPIO_MODE_INPUT)
                    {
                        data->inputMask[port] |= (uint16_t)pinConfig->Pin;
                    }
                }
            }

            data->config = config;
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
        const GPIO_PinState pinState = (level == HW_GPIO_LEVEL_HIGH) ? GPIO_PIN_SET : GPIO_PIN_RESET;
        HAL_GPIO_WritePin(HW_GPIO_portHandleMapping[port], (uint16_t)pin, pinState);
    }
}

// [impl->fw~hal_gpio_004~1]
void HW_GPIO_run1ms(void)
{
    // One IDR read per port that has input pins; mask down to the inputs.
    for (HW_GPIO_port_E port = 0U; port < HW_GPIO_PORT_COUNT; port++)
    {
        if (data->inputMask[port] != 0U)
        {
            const uint16_t idr = (uint16_t)HW_GPIO_portHandleMapping[port]->IDR;
            data->cachedInput[port] = (uint16_t)(idr & data->inputMask[port]);
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
        for (uint32_t bit = 0U; bit < 16U; bit++)
        {
            if ((pin & (1UL << bit)) != 0U)
            {
                data->extiTable[bit].port = port;
                data->extiTable[bit].callback = callback;
                data->extiTable[bit].context = context;
            }
        }
        ret = true;
    }
    return ret;
}

// Overrides the HAL's weak HAL_GPIO_EXTI_Callback; the HAL IRQ handler
// dispatches the pending line(s) here, and we fan out to the registered
// per-line callback.
// [impl->fw~hal_gpio_005~1]
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    for (uint32_t bit = 0U; bit < 16U; bit++)
    {
        if ((GPIO_Pin & (1U << bit)) != 0U)
        {
            const HW_GPIO_extiEntry_S * const entry = &data->extiTable[bit];
            if (entry->callback != NULL)
            {
                entry->callback(entry->port, (1UL << bit), entry->context);
            }
        }
    }
}
