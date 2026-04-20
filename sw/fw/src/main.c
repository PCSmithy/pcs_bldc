#include "lib_build.h"
#include "HW_systemClock.h"
#include "HW_ADC.h"

#if (BUILD_TARGET == BUILD_TARGET_STM32G4)
  #include "stm32g4xx_hal.h"  // HAL_Init
#endif

extern const HW_systemClock_config_S HW_systemClock_config;
extern const HW_ADC_config_S HW_ADC_config;


void Error_Handler(void)
{
    while (1)
    {
    }
}

int main(void)
{
    // TODO: channelize this into an HW_halCore module (stm32g4 + sim
    // impls) so main.c doesn't need a target-specific include or
    // BUILD_TARGET branch. For now, gate it.
#if (BUILD_TARGET == BUILD_TARGET_STM32G4)
    HAL_Init();
#endif

    bool initSuccess = true;
    initSuccess &= HW_systemClock_init(&HW_systemClock_config);
    initSuccess &= HW_ADC_init(&HW_ADC_config);

    if (!initSuccess)
    {
        Error_Handler();
    }
    return 0;
}
