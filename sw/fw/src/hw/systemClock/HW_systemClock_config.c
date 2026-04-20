

/* Includes */
#include "HW_systemClock.h"
#include "lib_build.h"  // BUILD_TARGET_STM32G4 / BUILD_TARGET_SIM constants

/* Defines */

/* Typedefs */

/* Private Function Declarations */

/* Private Data Definitions */
const HW_systemClock_config_S HW_systemClock_config =
{
#if (BUILD_TARGET == BUILD_TARGET_STM32G4)
    // TODO - update convert_cubemx_to_canonical.sh to populate this struct directly from cubemx generated output
    .RCC_OscInitStruct =
    {
        .OscillatorType = RCC_OSCILLATORTYPE_HSE,
        .HSEState = RCC_HSE_ON,
        .PLL.PLLState = RCC_PLL_ON,
        .PLL.PLLSource = RCC_PLLSOURCE_HSE,
        .PLL.PLLM = RCC_PLLM_DIV2,
        .PLL.PLLN = 24,
        .PLL.PLLP = RCC_PLLP_DIV2,
        .PLL.PLLQ = RCC_PLLQ_DIV6,
        .PLL.PLLR = RCC_PLLR_DIV2,
    },
    .RCC_ClkInitStruct =
    {
        .ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK|RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2,
        .SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK,
        .AHBCLKDivider = RCC_SYSCLK_DIV1,
        .APB1CLKDivider = RCC_HCLK_DIV1,
        .APB2CLKDivider = RCC_HCLK_DIV1,
    },
    .PWREx_ControlVoltageScaling = PWR_REGULATOR_VOLTAGE_SCALE1,
    .FLASH_latency = FLASH_LATENCY_4,
#elif (BUILD_TARGET == BUILD_TARGET_SIM)
    ._void = 0U,
#else
#error "ERROR! HW_systemClock_config not defined for build target!"
#endif
};

/* Private Function Definitions */

/* Public Function Definitions */
