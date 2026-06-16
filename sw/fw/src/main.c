#include "lib_build.h"
#include "HW_systemClock.h"
#include "HW_GPIO.h"
#include "HW_ADC.h"
#include "HW_SPI.h"

#if (BUILD_TARGET == BUILD_TARGET_STM32G4)
  #include "stm32g4xx_hal.h"  // HAL_Init
  #include "FreeRTOS.h"
  #include "task.h"
  #include "usb.h"
  // io/ is embedded-only (the tree pulls in tinyusb), so the encoder driver
  // is only available on this target.
  #include "IO_AS5048.h"
#endif

extern const HW_systemClock_config_S HW_systemClock_config;
extern const HW_GPIO_config_S HW_GPIO_config;
extern const HW_ADC_config_S HW_ADC_config;
extern const HW_SPI_config_S HW_SPI_config;

#if (BUILD_TARGET == BUILD_TARGET_STM32G4)
extern const IO_AS5048_config_S IO_AS5048_config;
#endif

#if (BUILD_TARGET == BUILD_TARGET_STM32G4)
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

// Fixed-rate 1 ms IO task. Home for periodic sensor/actuator run functions
// (encoder sampling now; the control loop will likely move to its own faster
// task later). vTaskDelayUntil gives a drift-free 1 ms cadence regardless of
// how long the body takes. High priority so sampling preempts USB servicing.
static void task_1ms(void * params)
{
    (void)params;
    TickType_t lastWake = xTaskGetTickCount();
    for (;;)
    {
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(1U));

        // hw

        // io
        IO_AS5048_run1ms();

        // dev

        // app
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
    initSuccess &= HW_SPI_init(&HW_SPI_config);
#if (BUILD_TARGET == BUILD_TARGET_STM32G4)
    initSuccess &= IO_AS5048_init(&IO_AS5048_config);
#endif

    if (!initSuccess)
    {
        Error_Handler();
    }

#if (BUILD_TARGET == BUILD_TARGET_STM32G4)
    // Spawn the 1 ms IO task (drives IO_AS5048_run1ms) and bring up USB CDC
    // (spawns the device task, which reads the latest cached angle and prints
    // it), then hand control to the scheduler. Both allocate their task from
    // the FreeRTOS heap; a failure here (e.g. heap exhaustion) must halt loudly
    // rather than silently drop a task. vTaskStartScheduler() does not return.
    bool tasksCreated = true;
    tasksCreated &= (xTaskCreate(task_1ms, "task_1ms", configMINIMAL_STACK_SIZE * 2U,
                                 NULL, configMAX_PRIORITIES - 1U, NULL) == pdPASS);
    tasksCreated &= USB_init();

    if (!tasksCreated)
    {
        Error_Handler();
    }

    vTaskStartScheduler();
#endif

    return 0;
}
