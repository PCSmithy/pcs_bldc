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
  // io/ is embedded-only (the tree pulls in tinyusb), so the encoder + LED
  // drivers are only available on this target.
  #include "IO_AS5048.h"
  #include "IO_SK6805.h"
#endif

extern const HW_systemClock_config_S HW_systemClock_config;
extern const HW_GPIO_config_S HW_GPIO_config;
extern const HW_ADC_config_S HW_ADC_config;
extern const HW_SPI_config_S HW_SPI_config;

#if (BUILD_TARGET == BUILD_TARGET_STM32G4)
extern const IO_AS5048_config_S IO_AS5048_config;
extern const IO_SK6805_config_S IO_SK6805_config;
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

// FreeRTOS task priority hierarchy (higher number preempts lower), defined in
// one place so the ordering is explicit. The encoder sampler is hard
// real-time. The LED refresh briefly blocks (~1.25 ms every 50 ms) but must
// hit its cadence regardless of USB load, so it outranks the USB servicer —
// the added USB latency is negligible. USB is event-driven and blocks when
// idle, so it sits lowest of the three.
#define TASK_PRIO_ENCODER  (configMAX_PRIORITIES - 1U)
#define TASK_PRIO_LED      (configMAX_PRIORITIES - 2U)
#define TASK_PRIO_USB      (configMAX_PRIORITIES - 3U)

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

// Low-rate LED task: bring-up animation (a single dim pixel walking the
// string) refreshed at ~20 Hz. Runs above the USB task (TASK_PRIO_LED) so its
// 50 ms cadence isn't held off by USB servicing — otherwise the animation
// stutters under USB load. A full refresh only blocks ~1.25 ms, so the USB
// latency cost is negligible.
//
// The SK6805 is a continuous timed stream: a task preemption mid-transfer
// (notably by the 1 ms task) underruns the SPI FIFO and corrupts the frame.
// vTaskSuspendAll() blocks task switches across the transmit so the stream
// stays intact; ISRs still run (short enough for the FIFO to ride out), and
// the encoder task just slips one ~1 ms sample. The real fix is DMA (no
// HW_DMA yet); this is the prototype path.
static void ledTask(void * params)
{
    (void)params;
    uint16_t pos = 0U;
    TickType_t lastWake = xTaskGetTickCount();
    for (;;)
    {
        IO_SK6805_clear();
        IO_SK6805_setPixel(pos, 16U, 0U, 0U);  // dim red, low current

        vTaskSuspendAll();
        (void)IO_SK6805_update();
        (void)xTaskResumeAll();

        pos = (uint16_t)((pos + 1U) % IO_SK6805_PIXEL_COUNT);
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(50U));
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
    initSuccess &= IO_SK6805_init(&IO_SK6805_config);
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
                                 NULL, TASK_PRIO_ENCODER, NULL) == pdPASS);
    tasksCreated &= (xTaskCreate(ledTask, "led", configMINIMAL_STACK_SIZE * 2U,
                                 NULL, TASK_PRIO_LED, NULL) == pdPASS);
    tasksCreated &= USB_init(TASK_PRIO_USB);

    if (!tasksCreated)
    {
        Error_Handler();
    }

    vTaskStartScheduler();
#endif

    return 0;
}
