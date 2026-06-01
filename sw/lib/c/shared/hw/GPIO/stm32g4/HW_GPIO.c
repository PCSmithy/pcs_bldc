/* Includes */
#include "HW_GPIO.h"
#include "stm32g4xx_hal.h"

/* Defines */

/* Typedefs */
typedef struct
{
    const HW_GPIO_config_S * config;
} HW_GPIO_data_S;

/* Private Function Declarations */

static void HW_GPIO_private_enablePortClock(HW_GPIO_port_E port);

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

/* Public Function Definitions */
bool HW_GPIO_init(const HW_GPIO_config_S * const config)
{
    bool ret = false;
    if (config != NULL)
    {
        bool success = true;
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
            }
        }

        if (success)
        {
            data->config = config;
            ret = true;
        }
    }
    return ret;
}

void HW_GPIO_writePin(HW_GPIO_port_E port, uint32_t pin, HW_GPIO_level_E level)
{
    if (port < HW_GPIO_PORT_COUNT)
    {
        const GPIO_PinState pinState = (level == HW_GPIO_LEVEL_HIGH) ? GPIO_PIN_SET : GPIO_PIN_RESET;
        HAL_GPIO_WritePin(HW_GPIO_portHandleMapping[port], (uint16_t)pin, pinState);
    }
}
