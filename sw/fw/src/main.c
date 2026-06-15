#include "lib_build.h"
#include "HW_systemClock.h"
#include "HW_GPIO.h"
#include "HW_ADC.h"

#if (BUILD_TARGET == BUILD_TARGET_STM32G4)
  #include "stm32g4xx_hal.h"  // HAL_Init
#endif

extern const HW_systemClock_config_S HW_systemClock_config;
extern const HW_GPIO_config_S HW_GPIO_config;
extern const HW_ADC_config_S HW_ADC_config;

#if (BUILD_TARGET == BUILD_TARGET_STM32G4)
// Weak placeholder so stm32g4xx_it.c's USB_LP_IRQHandler links before the USB
// CDC stack is integrated. usbd_conf.c will provide the real (strong)
// definition; until USB is initialized its interrupt never fires, so this
// zeroed handle is never actually used. TODO: remove when the USB stack lands.
__attribute__((weak)) PCD_HandleTypeDef hpcd_USB_FS;
#endif


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
    initSuccess &= HW_GPIO_init(&HW_GPIO_config);
    initSuccess &= HW_ADC_init(&HW_ADC_config);

    if (!initSuccess)
    {
        Error_Handler();
    }

#if (BUILD_TARGET == BUILD_TARGET_STM32G4)
    // TEMPORARY bring-up heartbeat: toggle ENC_SPI_CS0 (PC4) at ~1 Hz so a
    // scope/meter on connector J2 pin 3 confirms the board is flashed and
    // running. SPI is not yet initialized, so this CS line is inert and safe
    // to wiggle. Throwaway smoke test — delete once USB serial is up. To use
    // the TP22 test point instead, swap to GPIO_PIN_15 (DISABLE_VDS_PROT).
    HW_GPIO_level_E heartbeat = HW_GPIO_LEVEL_LOW;
    while (1)
    {
        HW_GPIO_writePin(HW_GPIO_PORT_C, GPIO_PIN_4, heartbeat);
        heartbeat = (heartbeat == HW_GPIO_LEVEL_LOW) ? HW_GPIO_LEVEL_HIGH : HW_GPIO_LEVEL_LOW;
        HAL_Delay(500U);
    }
#endif

    return 0;
}
