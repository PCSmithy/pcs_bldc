#include "lib_build.h"
#include "HW_systemClock.h"
#include "HW_GPIO.h"
#include "HW_ADC.h"
#include "HW_SPI.h"
#include "HW_TIM.h"
#include "HW_DMA.h"

#if (BUILD_TARGET == BUILD_TARGET_STM32G4)
  #include "stm32g4xx_hal.h"  // HAL_Init
  #include "FreeRTOS.h"
  #include "task.h"
  #include <stdio.h>
  #include "lib_utils.h"
  #include "lib_timer.h"
  #include "HW_USB.h"
  #include "IO_serial.h"
  // The tasks below need FreeRTOS, so the encoder, LED, switch, serial, and
  // telemetry wiring is only built for this target.
  #include "IO_AS5048.h"
  #include "IO_SK6805.h"
  #include "dev_switch.h"
  #include "app_rgbLedRing.h"
#endif

#if (BUILD_TARGET == BUILD_TARGET_SIM)
  // SIL bring-up: run FreeRTOS under the cooperative fiber port. Phase 2 will
  // hand tick-driving to the Rust framework via the control ABI; for now a
  // temporary in-process loop in main() advances the scheduler as a smoke test.
  #include <stdio.h>
  #include "FreeRTOS.h"
  #include "task.h"
#endif

extern const HW_systemClock_config_S HW_systemClock_config;
extern const HW_GPIO_config_S HW_GPIO_config;
extern const HW_ADC_config_S HW_ADC_config;
extern const HW_SPI_config_S HW_SPI_config;
extern const HW_TIM_config_S HW_TIM_config;
extern const HW_DMA_config_S HW_DMA_config;

#if (BUILD_TARGET == BUILD_TARGET_STM32G4)
extern const IO_AS5048_config_S IO_AS5048_config;
extern const IO_SK6805_config_S IO_SK6805_config;
extern const dev_switch_config_S dev_switch_config;
extern const IO_serial_config_S IO_serial_config;
extern const app_rgbLedRing_config_S app_rgbLedRing_config;
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

#define TASK_PRIORITY_1MS   (configMAX_PRIORITIES - 1U)
#define TASK_PRIORITY_10MS  (configMAX_PRIORITIES - 2U)
#define TASK_PRIORITY_USB   (configMAX_PRIORITIES - 3U)
#define TASK_PRIORITY_TELEM (configMAX_PRIORITIES - 4U)

// --- Task profiling (bring-up telemetry) -----------------------------------
// Each periodic task times its body against the microsecond time base and folds
// the duration into a per-task worst-case (max), which telemetryTask emits and
// resets every window. task_usb blocks on the USB event queue (not a periodic
// body), so it is not profiled.
typedef enum
{
    PROFILE_TASK_1MS,
    PROFILE_TASK_10MS,
    PROFILE_TASK_TELEM,
    PROFILE_TASK_COUNT,
} profileTask_E;

static volatile uint32_t profileMaxUs[PROFILE_TASK_COUNT];

// Fold one body execution's duration into the task's window max.
static void profileUpdate(profileTask_E task, uint32_t durationUs)
{
    if (durationUs > profileMaxUs[task])
    {
        profileMaxUs[task] = durationUs;
    }
}

// Snapshot the task's window max and clear it for the next window. The critical
// section makes the read-and-clear atomic against the (higher-priority)
// profiled tasks, so no sample is dropped between the read and the reset.
static uint32_t profileTakeMaxUs(profileTask_E task)
{
    taskENTER_CRITICAL();
    const uint32_t maxUs = profileMaxUs[task];
    profileMaxUs[task] = 0U;
    taskEXIT_CRITICAL();
    return maxUs;
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
        const uint32_t profileStartUs = (uint32_t)lib_timer_getTime_us();

        // hw
        HW_GPIO_run1ms();   // cache input-pin levels before anything reads them
        HW_ADC_run1ms();    // sample enabled ADC inputs (software-triggered, polled)

        // io
        IO_AS5048_run1ms();

        // dev
        dev_switch_run1ms();   // debounce switches off the cached GPIO snapshot

        // app

        profileUpdate(PROFILE_TASK_1MS, (uint32_t)lib_timer_getTime_us() - profileStartUs);
    }
}

static void task_10ms(void * params)
{
    (void)params;
    TickType_t lastWake = xTaskGetTickCount();
    for (;;)
    {
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(10U));
        const uint32_t profileStartUs = (uint32_t)lib_timer_getTime_us();

        // hw

        // io

        // dev

        // app
        app_rgbLedRing_run10ms();

        profileUpdate(PROFILE_TASK_10MS, (uint32_t)lib_timer_getTime_us() - profileStartUs);
    }
}

// Dedicated USB device-service task. HW_USB_run() blocks on the TinyUSB event
// queue and wakes on the USB ISR, so the stack is serviced on demand rather than
// polled. Sits below the periodic tasks (they preempt it) so its variable,
// load-dependent work never adds jitter to their cadence.
static void task_usb(void * params)
{
    (void)params;
    for (;;)
    {
        HW_USB_run();
    }
}

// Teleplot wire format (beacon):
#define TP_UNIT "\xC2\xA7"

// Telemetry emit period (ms).
#define TELEMETRY_PERIOD_MS 25

// One window's Teleplot packets are formatted into a buffer this size and pushed
// with a single IO_serial_write, so the CDC FIFO is flushed once per window
// rather than once per byte (the per-byte path is far too slow to keep up).
#define TELEMETRY_TX_BUF_BYTES 512U

static void telemetryTask(void * params)
{
    (void) params;

    // One window's packets are batched into txBuf and written once (see
    // TELEMETRY_TX_BUF_BYTES). Static to keep it off the task's small stack.
    static char txBuf[TELEMETRY_TX_BUF_BYTES];

    TickType_t lastWake = xTaskGetTickCount();
    for (;;)
    {
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(TELEMETRY_PERIOD_MS));

        // Nothing is listening until a host opens the port — skip the work.
        if (!IO_serial_connected(IO_SERIAL_CHANNEL_CDC))
        {
            continue;
        }

        const uint32_t profileStartUs = (uint32_t)lib_timer_getTime_us();

        // Source timestamp (ms) shared by every signal in this pass. FreeRTOS
        // owns SysTick, so HAL_GetTick() stays 0 — use the RTOS tick (1 ms).
        const uint32_t nowMs = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

        uint16_t motorAngleRaw = 0U;
        float32_t motorAngle_deg = 0.0f;
        if (!IO_AS5048_readAngle(IO_AS5048_CHANNEL_MOTOR, &motorAngleRaw, &motorAngle_deg))
        {
            motorAngleRaw = 0U;
            motorAngle_deg = 0.0f;
        }

        uint32_t motorAngleScaled = 0U;
        uint32_t motorAngleDecimalScaled = 0U;
        floatToFixed(motorAngle_deg, 100U, &motorAngleScaled, &motorAngleDecimalScaled);

        uint16_t dialAngleRaw = 0U;
        float32_t dialAngle_deg = 0.0f;
        if (!IO_AS5048_readAngle(IO_AS5048_CHANNEL_DIAL, &dialAngleRaw, &dialAngle_deg))
        {
            dialAngleRaw = 0U;
            dialAngle_deg = 0.0f;
        }

        uint32_t dialAngleScaled = 0U;
        uint32_t dialAngleDecimalScaled = 0U;
        floatToFixed(dialAngle_deg, 100U, &dialAngleScaled, &dialAngleDecimalScaled);

        // Batch every packet into txBuf, then push it with one write. Each
        // snprintf only advances the offset if the packet fit (guards against a
        // truncated write corrupting the length).
        int off = 0;
        int n = snprintf(&txBuf[off], sizeof(txBuf) - (size_t)off,
                         "motor_angle:%lu:%lu.%02lu" TP_UNIT "deg;"
                         "motor_raw:%lu:%u;"
                         "dial_angle:%lu:%lu.%02lu" TP_UNIT "deg;"
                         "dial_raw:%lu:%u\n",
                         (unsigned long)nowMs,
                         (unsigned long)motorAngleScaled,
                         (unsigned long)motorAngleDecimalScaled,
                         (unsigned long)nowMs,
                         (unsigned)motorAngleRaw,
                         (unsigned long)nowMs,
                         (unsigned long)dialAngleScaled,
                         (unsigned long)dialAngleDecimalScaled,
                         (unsigned long)nowMs,
                         (unsigned)dialAngleRaw);
        if ((n > 0) && ((size_t)n < (sizeof(txBuf) - (size_t)off))) { off += n; }

        // uint32_t  adc1Count = 0U;
        // float32_t adc1Volts = 0.0f;
        // (void)HW_ADC_getCount(HW_ADC_CHANNEL_1, 6U, &adc1Count);
        // (void)HW_ADC_getVolts(HW_ADC_CHANNEL_1, 6U, &adc1Volts);

        // uint32_t  adc2Count = 0U;
        // float32_t adc2Volts = 0.0f;
        // (void)HW_ADC_getCount(HW_ADC_CHANNEL_2, 11U, &adc2Count);
        // (void)HW_ADC_getVolts(HW_ADC_CHANNEL_2, 11U, &adc2Volts);

        // uint32_t adc1Whole = 0U;
        // uint32_t adc1Frac  = 0U;
        // floatToFixed(adc1Volts, 1000U, &adc1Whole, &adc1Frac);
        // uint32_t adc2Whole = 0U;
        // uint32_t adc2Frac  = 0U;
        // floatToFixed(adc2Volts, 1000U, &adc2Whole, &adc2Frac);

        // HW_ADC_conversionStatus_E adc1Status = HW_ADC_CONVERSION_STATUS_IDLE;
        // HW_ADC_conversionStatus_E adc2Status = HW_ADC_CONVERSION_STATUS_IDLE;
        // (void)HW_ADC_getStatus(HW_ADC_CHANNEL_1, &adc1Status);
        // (void)HW_ADC_getStatus(HW_ADC_CHANNEL_2, &adc2Status);

        // printf("adc1_cnt:%lu:%lu;"
        //        "adc1_v:%lu:%lu.%03lu" TP_UNIT "V;"
        //        "adc1_status:%lu:%u\n",
        //         (unsigned long)nowMs,
        //         (unsigned long)adc1Count,
        //         (unsigned long)nowMs,
        //         (unsigned long)adc1Whole,
        //         (unsigned long)adc1Frac,
        //         (unsigned long)nowMs,
        //         (unsigned)adc1Status);
        // printf("adc2_cnt:%lu:%lu;"
        //        "adc2_v:%lu:%lu.%03lu" TP_UNIT "V;"
        //        "adc2_status:%lu:%u\n",
        //         (unsigned long)nowMs,
        //         (unsigned long)adc2Count,
        //         (unsigned long)nowMs,
        //         (unsigned long)adc2Whole,
        //         (unsigned long)adc2Frac,
        //         (unsigned long)nowMs,
        //         (unsigned)adc2Status);

        // Per-task worst-case body duration since the last emit (microseconds),
        // snapshotted and reset each window. task1ms/task10ms are pure CPU time
        // (their bodies never block); telem is wall-clock and so includes any
        // CDC backpressure waits.
        const uint32_t task1msMaxUs  = profileTakeMaxUs(PROFILE_TASK_1MS);
        const uint32_t task10msMaxUs = profileTakeMaxUs(PROFILE_TASK_10MS);
        const uint32_t telemMaxUs    = profileTakeMaxUs(PROFILE_TASK_TELEM);

        n = snprintf(&txBuf[off], sizeof(txBuf) - (size_t)off,
                     "task1ms_us:%lu:%lu" TP_UNIT "us;"
                     "task10ms_us:%lu:%lu" TP_UNIT "us;"
                     "telem_us:%lu:%lu" TP_UNIT "us\n",
                     (unsigned long)nowMs, (unsigned long)task1msMaxUs,
                     (unsigned long)nowMs, (unsigned long)task10msMaxUs,
                     (unsigned long)nowMs, (unsigned long)telemMaxUs);
        if ((n > 0) && ((size_t)n < (sizeof(txBuf) - (size_t)off))) { off += n; }

        // One write per window: IO_serial_write flushes the CDC FIFO once at the
        // end, instead of the per-byte flush the printf path incurred.
        if (off > 0)
        {
            IO_serial_write(IO_SERIAL_CHANNEL_CDC, (const uint8_t *)txBuf, (uint32_t)off);
        }

        profileUpdate(PROFILE_TASK_TELEM, (uint32_t)lib_timer_getTime_us() - profileStartUs);
    }
}

// Retarget printf to the CDC serial channel. syscalls.c's weak _write calls
// __io_putchar; IO_serial_write applies the backpressure/yield.
int __io_putchar(int ch)
{
    const uint8_t c = (uint8_t) ch;
    IO_serial_write(IO_SERIAL_CHANNEL_CDC, &c, 1U);
    return ch;
}
#endif

// HW-layer init, shared by both targets' entry paths.
static bool prvHwInit(void)
{
    bool ok = true;
    ok &= HW_systemClock_init(&HW_systemClock_config);
    ok &= HW_GPIO_init(&HW_GPIO_config);
    ok &= HW_ADC_init(&HW_ADC_config);
    ok &= HW_DMA_init(&HW_DMA_config);   // before SPI: SPI registers DMA completion callbacks
    ok &= HW_SPI_init(&HW_SPI_config);
    ok &= HW_TIM_init(&HW_TIM_config);
    return ok;
}

#if (BUILD_TARGET == BUILD_TARGET_SIM)
#include "sil_fw.h"

// Native fiber-port primitives (provided by the cooperative fiber port).
extern void vSilAdvanceTick(void);
extern void vPortYieldToScheduler(void);

// Quiescence handoff: when every task is blocked the idle task runs and hands
// control back to the driver (framework) fiber.
void vApplicationIdleHook(void)
{
    vPortYieldToScheduler();
}

// Minimal SIL 1 ms task: drives the HW-layer periodic sampling. Stands in for
// the real io/dev/app tasks until they're ungated for SIM. sim_task1msRuns is
// observable from the framework via the State Table.
volatile uint32_t sim_task1msRuns = 0U;
static void sim_task_1ms(void * params)
{
    (void)params;
    TickType_t lastWake = xTaskGetTickCount();
    for (;;)
    {
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(1U));
        HW_GPIO_run1ms();
        HW_ADC_run1ms();
        sim_task1msRuns++;
    }
}

// --- SIL control ABI (D2) --------------------------------------------------
// The framework drives these; pacing (fast vs realtime) is the driver's choice.

bool sil_fw_start(void)
{
    bool ok = prvHwInit();
    ok = ok && ( xTaskCreate(sim_task_1ms, "sim_1ms", configMINIMAL_STACK_SIZE,
                             NULL, 3, NULL) == pdPASS );
    if (ok)
    {
        // Fiber port: runs to first quiescence (all tasks blocked) and returns.
        vTaskStartScheduler();
    }
    return ok;
}

void sil_fw_advance_tick(void)
{
    vSilAdvanceTick();
}

void sil_fw_shutdown(void)
{
    vPortEndScheduler();
}
#endif

#if (BUILD_TARGET == BUILD_TARGET_STM32G4)
int main(void)
{
    // TODO: channelize HAL_Init into an HW_halCore module so main.c doesn't
    // need a target-specific include. For now, gate it.
    HAL_Init();

    bool initSuccess = prvHwInit();
    initSuccess &= IO_AS5048_init(&IO_AS5048_config);
    initSuccess &= IO_SK6805_init(&IO_SK6805_config);
    initSuccess &= dev_switch_init(&dev_switch_config);
    initSuccess &= app_rgbLedRing_init(&app_rgbLedRing_config);
    initSuccess &= HW_USB_init();   // USB device stack (serviced in task_1ms)
    initSuccess &= IO_serial_init(&IO_serial_config);
    if (!initSuccess)
    {
        Error_Handler();
    }

    // Spawn the periodic tasks and hand control to the scheduler. Each task
    // allocates from the FreeRTOS heap; a failure here (e.g. heap exhaustion)
    // must halt loudly rather than silently drop a task. vTaskStartScheduler()
    // does not return.
    bool tasksCreated = (xTaskCreate(task_1ms, "task_1ms", configMINIMAL_STACK_SIZE * 2U,
                                     NULL, TASK_PRIORITY_1MS, NULL) == pdPASS);
    tasksCreated &= (xTaskCreate(task_10ms, "task_10ms", configMINIMAL_STACK_SIZE * 2U,
                                 NULL, TASK_PRIORITY_10MS, NULL) == pdPASS);
    tasksCreated &= (xTaskCreate(task_usb, "usbd", configMINIMAL_STACK_SIZE * 2U,
                                 NULL, TASK_PRIORITY_USB, NULL) == pdPASS);
    tasksCreated &= (xTaskCreate(telemetryTask, "telem", 512U,
                                 NULL, TASK_PRIORITY_TELEM, NULL) == pdPASS);

    if (!tasksCreated)
    {
        Error_Handler();
    }

    vTaskStartScheduler();
    return 0;
}
#endif

#if (BUILD_TARGET == BUILD_TARGET_SIM)
// Standalone SIL smoke driver over the control ABI (sil_fw.h). In Phase 2 the
// Rust framework drives the same three calls (and owns pacing); this loop is a
// temporary stand-in driver. Confirms the scheduler runs the task and the ADC
// sim ramp advances tick-over-tick.
int main(void)
{
    if (!sil_fw_start())
    {
        return 1;
    }

    // Find the first enabled ADC (channel, input) to watch the sim ramp on.
    HW_ADC_channels_E watchCh = (HW_ADC_channels_E)0;
    uint8_t watchIn = 0U;
    for (uint32_t ch = 0U; ch < (uint32_t)HW_ADC_CHANNEL_COUNT; ch++)
    {
        bool found = false;
        for (uint8_t in = 0U; in < HW_ADC_INPUTS_PER_CHANNEL; in++)
        {
            uint32_t tmp = 0U;
            if (HW_ADC_getCount((HW_ADC_channels_E)ch, in, &tmp))
            {
                watchCh = (HW_ADC_channels_E)ch;
                watchIn = in;
                found = true;
                break;
            }
        }
        if (found)
        {
            break;
        }
    }

    for (uint32_t tick = 1U; tick <= 20U; tick++)
    {
        sil_fw_advance_tick();

        uint32_t counts = 0U;
        (void)HW_ADC_getCount(watchCh, watchIn, &counts);
        printf("tick %2u  task_runs=%u  adc[ch%u,in%u]=%u\n",
               tick, sim_task1msRuns, (unsigned)watchCh, watchIn, counts);
    }

    sil_fw_shutdown();
    return 0;
}
#endif
