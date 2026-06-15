#include "lib_build.h"
#include "HW_systemClock.h"
#include "HW_GPIO.h"
#include "HW_ADC.h"

#if (BUILD_TARGET == BUILD_TARGET_STM32G4)
  #include "stm32g4xx_hal.h"  // HAL_Init
  #include "FreeRTOS.h"
  #include "task.h"
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
// stm32g4xx_it.c's TIM6_DAC_IRQHandler references hdac1; the DAC isn't
// integrated, so a zeroed weak handle lets it.o link (the DAC interrupt never
// fires). TODO: remove when a DAC driver lands.
__attribute__((weak)) DAC_HandleTypeDef hdac1;
#endif


void Error_Handler(void)
{
    while (1)
    {
    }
}

#if (BUILD_TARGET == BUILD_TARGET_STM32G4)
// FreeRTOS static-allocation memory (configSUPPORT_STATIC_ALLOCATION=1).
// cmsis_os2.c normally provides these; we supply them since we use the native
// FreeRTOS API without the CMSIS-RTOS wrapper.
static StaticTask_t idleTaskTcb;
static StackType_t  idleTaskStack[configMINIMAL_STACK_SIZE];
void vApplicationGetIdleTaskMemory(StaticTask_t ** ppxTcb, StackType_t ** ppxStack, uint32_t * pulSize)
{
    *ppxTcb   = &idleTaskTcb;
    *ppxStack = idleTaskStack;
    *pulSize  = configMINIMAL_STACK_SIZE;
}

static StaticTask_t timerTaskTcb;
static StackType_t  timerTaskStack[configTIMER_TASK_STACK_DEPTH];
void vApplicationGetTimerTaskMemory(StaticTask_t ** ppxTcb, StackType_t ** ppxStack, uint32_t * pulSize)
{
    *ppxTcb   = &timerTaskTcb;
    *ppxStack = timerTaskStack;
    *pulSize  = configTIMER_TASK_STACK_DEPTH;
}

// TEMPORARY bring-up heartbeat task: toggle ENC_SPI_CS0 (PC4) at ~1 Hz so a
// scope/debugger confirms the scheduler is running. Throwaway smoke test —
// delete once USB serial is up.
static void heartbeatTask(void * params)
{
    (void)params;
    HW_GPIO_level_E level = HW_GPIO_LEVEL_LOW;
    for (;;)
    {
        HW_GPIO_writePin(HW_GPIO_PORT_C, GPIO_PIN_4, level);
        level = (level == HW_GPIO_LEVEL_LOW) ? HW_GPIO_LEVEL_HIGH : HW_GPIO_LEVEL_LOW;
        vTaskDelay(pdMS_TO_TICKS(500U));
    }
}
#endif

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
    // Spawn the bring-up heartbeat task and hand control to the scheduler.
    // vTaskStartScheduler() does not return.
    (void)xTaskCreate(heartbeatTask, "heartbeat", configMINIMAL_STACK_SIZE,
                      NULL, tskIDLE_PRIORITY + 1U, NULL);
    vTaskStartScheduler();
#endif

    return 0;
}
