/* Includes */

#include "lib_types.h"
#include "lib_utils.h"
#include "lib_build.h"

#include "HW_GPIO.h"

/* Defines */

/* Typedefs */

/* Private Data Definitions */

#if (BUILD_TARGET == BUILD_TARGET_STM32G4)

// Per-port pin arrays. CubeMX-derived from MX_GPIO_Init in
// sw/fw/stm32cube/g4/Core/Src/main.c. Each entry is a HAL
// GPIO_InitTypeDef; HW_GPIO_init iterates these and calls HAL_GPIO_Init
// for each one (after enabling the port's RCC clock).
//
// PB6 — EXTI rising-edge input.
static const GPIO_InitTypeDef HW_GPIO_portBPinConfigs[] =
{
    {
        .Pin   = GPIO_PIN_6,
        .Mode  = GPIO_MODE_IT_RISING,
        .Pull  = GPIO_NOPULL,
    },
};

// PC13 — EXTI rising-edge input.
// PC14 — plain digital input.
// PC15, PC4 — push-pull outputs (low speed). Initial level RESET (set by
//             HW_GPIO_init before HAL_GPIO_Init flips the pin to output).
static const GPIO_InitTypeDef HW_GPIO_portCPinConfigs[] =
{
    {
        .Pin   = GPIO_PIN_13,
        .Mode  = GPIO_MODE_IT_RISING,
        .Pull  = GPIO_NOPULL,
    },
    {
        .Pin   = GPIO_PIN_14,
        .Mode  = GPIO_MODE_INPUT,
        .Pull  = GPIO_NOPULL,
    },
    {
        .Pin   = GPIO_PIN_15 | GPIO_PIN_4,
        .Mode  = GPIO_MODE_OUTPUT_PP,
        .Pull  = GPIO_NOPULL,
        .Speed = GPIO_SPEED_FREQ_LOW,
    },
};

const HW_GPIO_config_S HW_GPIO_config =
{
    .ports =
    {
        [HW_GPIO_PORT_B] =
        {
            .pins    = HW_GPIO_portBPinConfigs,
            .numPins = COUNTOF(HW_GPIO_portBPinConfigs),
        },
        [HW_GPIO_PORT_C] =
        {
            .pins    = HW_GPIO_portCPinConfigs,
            .numPins = COUNTOF(HW_GPIO_portCPinConfigs),
        },
    },
};

#elif (BUILD_TARGET == BUILD_TARGET_SIM)

static const HW_GPIO_pinConfig_S HW_GPIO_portBPinConfigs[] =
{
    { .pinNameStr = "PB6" }, // (EXTI)
};

static const HW_GPIO_pinConfig_S HW_GPIO_portCPinConfigs[] =
{
    { .pinNameStr = "PC4" },    // (output)
    { .pinNameStr = "PC13" },   // (EXTI)
    { .pinNameStr = "PC14" },   // (input)
    { .pinNameStr = "PC15" },   // (output)
};

const HW_GPIO_config_S HW_GPIO_config =
{
    .ports =
    {
        [HW_GPIO_PORT_B] =
        {
            .pins    = HW_GPIO_portBPinConfigs,
            .numPins = COUNTOF(HW_GPIO_portBPinConfigs),
        },
        [HW_GPIO_PORT_C] =
        {
            .pins    = HW_GPIO_portCPinConfigs,
            .numPins = COUNTOF(HW_GPIO_portCPinConfigs),
        },
    },
};

#else
#error "ERROR! HW_GPIO_config not defined for build target!"
#endif

/* Private Function Definitions */

/* Public Function Definitions */
