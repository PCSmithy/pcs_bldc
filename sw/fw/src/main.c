#include "lib_build.h"
#include "HW_systemClock.h"
#include "HW_GPIO.h"
#include "HW_OPAMP.h"
#include "HW_ADC.h"
#include "HW_SPI.h"
#include "HW_I2C.h"
#include "HW_TIM.h"
#include "HW_DMA.h"

// Task creation, the periodic tasks, and every io/dev/app module they drive are
// target-uniform: the SIL (native) build runs the SAME FreeRTOS tasks against
// the sim HW drivers that the embedded build runs against the STM32G4 drivers.
// Target divergence lives only at the hw-layer seam and in the small gated
// blocks below (HAL bring-up, the HAL timebase callback, printf retarget, and
// each target's entry path).
#include <stdio.h>
#include "FreeRTOS.h"
#include "task.h"
#include "lib_utils.h"
#include "lib_timer.h"
#include "HW_USB.h"
#include "IO_serial.h"
#include "IO_COBSFrame.h"
#include "IO_AS5048.h"
#include "IO_SK6805.h"
#include "IO_i2c.h"
#include "IO_bridge.h"
#include "dev_switch.h"
#include "dev_CYPD3177.h"
#include "dev_gateDriver.h"
#include "app_rgbLedRing.h"
#include "app_motorControl.h"
#include "app_userControls.h"
#include "app_server.h"

#if (BUILD_TARGET == BUILD_TARGET_STM32G4)
  #include "stm32g4xx_hal.h"  // HAL_Init
#endif

extern const HW_systemClock_config_S HW_systemClock_config;
extern const HW_GPIO_config_S HW_GPIO_config;
extern const HW_OPAMP_config_S HW_OPAMP_config;
extern const HW_ADC_config_S HW_ADC_config;
extern const HW_SPI_config_S HW_SPI_config;
extern const HW_I2C_config_S HW_I2C_config;
extern const HW_TIM_config_S HW_TIM_config;
extern const HW_DMA_config_S HW_DMA_config;
extern const IO_i2c_config_S IO_i2c_config;
extern const IO_bridge_config_S IO_bridge_config;

extern const IO_AS5048_config_S IO_AS5048_config;
extern const IO_SK6805_config_S IO_SK6805_config;
extern const dev_switch_config_S dev_switch_config;
extern const dev_CYPD3177_config_S dev_CYPD3177_config;
extern const dev_gateDriver_config_S dev_gateDriver_config;
extern const IO_serial_config_S IO_serial_config;
extern const IO_COBSFrame_config_S IO_COBSFrame_config;
extern const app_rgbLedRing_config_S app_rgbLedRing_config;
extern const app_motorControl_config_S app_motorControl_config;
extern const app_userControls_config_S app_userControls_config;

#if (BUILD_TARGET == BUILD_TARGET_STM32G4)
// stm32g4xx_it.c's TIM6_DAC_IRQHandler references hdac1; the DAC isn't
// integrated, so a zeroed weak handle lets it.o link (the DAC interrupt never
// fires). TODO: remove when a DAC driver lands.
__attribute__((weak)) DAC_HandleTypeDef hdac1;

// The HAL time base runs on TIM6 (stm32g4xx_hal_timebase_tim.c), leaving
// SysTick to FreeRTOS. TIM6_DAC_IRQHandler -> HAL_TIM_IRQHandler fires this
// callback each 1 ms; without it HAL_IncTick is never called, uwTick stays
// frozen at 0, and every HAL timeout (ADC/SPI/I2C PollForX) waits forever
// instead of bounding. CubeMX emits this in its main.c, which this project
// doesn't vendor, so it lives here.
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef * htim)
{
    if (htim->Instance == TIM6)
    {
        HAL_IncTick();
    }
}
#endif


void Error_Handler(void)
{
    while (1)
    {
    }
}

// FreeRTOS static-allocation memory providers (configSUPPORT_STATIC_ALLOCATION=1).
// cmsis_os2.c normally supplies these; we provide them since we use the native
// FreeRTOS API without the CMSIS-RTOS wrapper. Target-uniform: the board config
// and the SIL fiber-port config both enable static allocation, and the kernel
// creates the idle task (and, where enabled, the timer task) through these on
// either target. The timer-task provider is only needed when software timers
// are compiled in (configUSE_TIMERS).
#if (configSUPPORT_STATIC_ALLOCATION == 1)
static StaticTask_t idleTaskTcb;
static StackType_t  idleTaskStack[configMINIMAL_STACK_SIZE];
void vApplicationGetIdleTaskMemory(StaticTask_t ** ppxTcb, StackType_t ** ppxStack, uint32_t * pulSize)
{
    *ppxTcb   = &idleTaskTcb;
    *ppxStack = idleTaskStack;
    *pulSize  = configMINIMAL_STACK_SIZE;
}

#if (configUSE_TIMERS == 1)
static StaticTask_t timerTaskTcb;
static StackType_t  timerTaskStack[configTIMER_TASK_STACK_DEPTH];
void vApplicationGetTimerTaskMemory(StaticTask_t ** ppxTcb, StackType_t ** ppxStack, uint32_t * pulSize)
{
    *ppxTcb   = &timerTaskTcb;
    *ppxStack = timerTaskStack;
    *pulSize  = configTIMER_TASK_STACK_DEPTH;
}
#endif
#endif

#define TASK_PRIORITY_1MS   (configMAX_PRIORITIES - 1U)
#define TASK_PRIORITY_10MS  (configMAX_PRIORITIES - 2U)
#define TASK_PRIORITY_USB   (configMAX_PRIORITIES - 3U)
#define TASK_PRIORITY_SERVER (configMAX_PRIORITIES - 4U)
#define TASK_PRIORITY_200MS (configMAX_PRIORITIES - 5U)

// --- Task profiling --------------------------------------------------------
// Each periodic task times its body against the microsecond time base and folds
// the duration into a per-task worst-case (max). Read/reset via
// profileTakeMaxUs, and readable externally by DWARF path (candidate State
// Table / signal-trace signals). task_usb blocks on the USB event queue (not a
// periodic body), so it is not profiled.
typedef enum
{
    PROFILE_TASK_1MS,
    PROFILE_TASK_10MS,
    PROFILE_TASK_200MS,
    PROFILE_TASK_SERVER,
    PROFILE_TASK_COUNT,
} profileTask_E;

static volatile uint32_t profileMaxUs[PROFILE_TASK_COUNT];

// --- Per-task heartbeat counters (SIL liveness) ----------------------------
// One free-running counter per task, bumped once per loop-body iteration.
// Unlike profileMaxUs (which resets every telemetry window), these are
// monotonic — the SIL driver reads them by DWARF path to prove each real task
// is actually advancing on the native scheduler, and they are candidate State
// Table signals. Target-uniform; volatile so the external (DLL/DWARF) view is
// never stale.
static volatile uint32_t task1msRuns;
static volatile uint32_t task10msRuns;
static volatile uint32_t task200msRuns;
static volatile uint32_t taskUsbRuns;
static volatile uint32_t serverRuns;

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

static void task_1ms(void * params)
{
    (void)params;
    TickType_t lastWake = xTaskGetTickCount();
    for (;;)
    {
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(1U));
        task1msRuns++;
        const uint32_t profileStartUs = (uint32_t)lib_timer_getTime_us();

        // hw
        HW_GPIO_run1ms();   // cache input-pin levels before anything reads them
        HW_ADC_run1ms();    // sample enabled ADC inputs (software-triggered, polled)

        // io
        IO_AS5048_run1ms();

        // dev
        dev_switch_run1ms();   // debounce switches off the cached GPIO snapshot

        // app
        app_userControls_run1ms();   // button + dial -> motor mode/velocity commands
        app_motorControl_run1ms();   // in-module overcurrent trip + enable gating (fw~safety_001 / fw~mc_006)
#if (BUILD_TARGET == BUILD_TARGET_SIM)
        {
            // Sim trace window word [0]: the SIL trace scenarios' 1 kHz signal.
            extern uint32_t app_server_simTraceWindow32[];
            app_server_simTraceWindow32[0]++;
        }
#endif
        app_server_sample1ms();      // capture trace watches after the control update (fw~conn_trace_004)

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
        task10msRuns++;
        const uint32_t profileStartUs = (uint32_t)lib_timer_getTime_us();

        // hw

        // io

        // dev

        // app
        app_rgbLedRing_run10ms();

        profileUpdate(PROFILE_TASK_10MS, (uint32_t)lib_timer_getTime_us() - profileStartUs);
    }
}

// Slow background sampling. Hosts blocking work (the PD-sink I2C poll), so it
// runs at the lowest priority — every real-time task preempts it.
static void task_200ms(void * params)
{
    (void)params;
    TickType_t lastWake = xTaskGetTickCount();
    for (;;)
    {
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(200U));
        task200msRuns++;
        const uint32_t profileStartUs = (uint32_t)lib_timer_getTime_us();

        // hw

        // io

        // dev
        dev_CYPD3177_run200ms();
        dev_gateDriver_run200ms();

        // The gate driver holds nFAULT low until it is configured, so TIM1
        // latches a break flag every boot. Clear that stale latch once the
        // driver first reports operational; later latches are real faults.
        static bool bridgeBreakLatchCleared = false;
        if ((!bridgeBreakLatchCleared) &&
            (dev_gateDriver_isOperational(DEV_GATEDRIVER_CHANNEL_MAIN)))
        {
            bridgeBreakLatchCleared = IO_bridge_clearBreakFlags(IO_BRIDGE_CHANNEL_MOTOR);
        }

        // app

        // 1 Hz heartbeat through printf: exercises the log stream end to end
        // (fw~obs_log_001/002) and gives any bench session a liveness line.
        if ((task200msRuns % 5U) == 0U)
        {
            printf("heartbeat %lus up, server %lu runs\n",
                   (unsigned long)(task200msRuns / 5U),
                   (unsigned long)serverRuns);
        }

        profileUpdate(PROFILE_TASK_200MS, (uint32_t)lib_timer_getTime_us() - profileStartUs);
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
        taskUsbRuns++;
        HW_USB_run();
    }
}


// Protocol server task: pump received frames, answer requests, publish the
// 10 Hz Status, drain the printf log stream. 1 ms cadence below the periodic
// control tasks, so serving the host never delays sampling or commutation.
static void task_server(void * params)
{
    (void)params;
    TickType_t lastWake = xTaskGetTickCount();
    for (;;)
    {
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(1U));
        serverRuns++;
        const uint32_t profileStartUs = (uint32_t)lib_timer_getTime_us();

        app_server_run1ms();

        profileUpdate(PROFILE_TASK_SERVER, (uint32_t)lib_timer_getTime_us() - profileStartUs);
    }
}

#if (BUILD_TARGET == BUILD_TARGET_STM32G4)
// Retarget printf into the server's log capture (fw~obs_log_001): syscalls.c's
// weak _write calls __io_putchar. Native uses its own libc stdio, so this hook
// is embedded-only.
int __io_putchar(int ch)
{
    app_server_logByte((uint8_t) ch);
    return ch;
}
#endif

// HW-layer init, shared by both targets' entry paths.
static bool prvHwInit(void)
{
    bool ok = true;
    ok &= HW_systemClock_init(&HW_systemClock_config);
    ok &= HW_GPIO_init(&HW_GPIO_config);
    ok &= HW_OPAMP_init(&HW_OPAMP_config);   // before ADC: op-amps must be calibrated and running before the ADC samples their internal outputs
    ok &= HW_ADC_init(&HW_ADC_config);
    ok &= HW_DMA_init(&HW_DMA_config);   // before SPI: SPI registers DMA completion callbacks
    ok &= HW_SPI_init(&HW_SPI_config);
    ok &= HW_I2C_init(&HW_I2C_config);
    ok &= HW_TIM_init(&HW_TIM_config);
    return ok;
}

// IO/dev/app-layer init, shared by both targets' entry paths. Aggregates each
// module's bool the same way prvHwInit does; main.c stays the only caller of
// Error_Handler.
static bool prvAppInit(void)
{
    bool ok = true;
    ok &= IO_AS5048_init(&IO_AS5048_config);
    ok &= IO_SK6805_init(&IO_SK6805_config);
    ok &= IO_i2c_init(&IO_i2c_config);
    ok &= IO_bridge_init(&IO_bridge_config);
    ok &= dev_switch_init(&dev_switch_config);
    ok &= dev_CYPD3177_init(&dev_CYPD3177_config);
    ok &= dev_gateDriver_init(&dev_gateDriver_config);
    ok &= app_rgbLedRing_init(&app_rgbLedRing_config);
    ok &= app_motorControl_init(&app_motorControl_config);
    ok &= app_userControls_init(&app_userControls_config);
    ok &= HW_USB_init();   // USB device stack (serviced in task_usb)
    ok &= IO_serial_init(&IO_serial_config);
    ok &= IO_COBSFrame_init(&IO_COBSFrame_config);
    ok &= app_server_init(&app_server_config);
    return ok;
}

// Spawn the periodic tasks. Same names/priorities/stacks on both targets; each
// allocates from the FreeRTOS heap, so a failure here (e.g. heap exhaustion) is
// surfaced to the caller to halt loudly rather than silently drop a task.
static bool prvCreateTasks(void)
{
    bool ok = (xTaskCreate(task_1ms, "task_1ms", configMINIMAL_STACK_SIZE * 2U,
                           NULL, TASK_PRIORITY_1MS, NULL) == pdPASS);
    ok &= (xTaskCreate(task_10ms, "task_10ms", configMINIMAL_STACK_SIZE * 2U,
                       NULL, TASK_PRIORITY_10MS, NULL) == pdPASS);
    ok &= (xTaskCreate(task_usb, "usbd", configMINIMAL_STACK_SIZE * 2U,
                       NULL, TASK_PRIORITY_USB, NULL) == pdPASS);
    ok &= (xTaskCreate(task_server, "server", 512U,
                       NULL, TASK_PRIORITY_SERVER, NULL) == pdPASS);
    ok &= (xTaskCreate(task_200ms, "task_200ms", configMINIMAL_STACK_SIZE * 2U,
                       NULL, TASK_PRIORITY_200MS, NULL) == pdPASS);
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

// --- SIL control ABI (D2) --------------------------------------------------
// The framework drives these; pacing (fast vs realtime) is the driver's choice.
// The bring-up path is identical to the embedded main() below (minus HAL_Init):
// the SAME HW/app init and the SAME four tasks. The fiber port runs the
// scheduler to first quiescence and returns.

void sil_fw_setHooks(const SIL_ports_hooks_S * const hooks)
{
    SIL_ports_setHooks(hooks);
}

bool sil_fw_start(void)
{
    bool ok = prvHwInit();
    ok = ok && prvAppInit();
    ok = ok && prvCreateTasks();
    if (ok)
    {
        // Fiber port: runs to first quiescence (all tasks blocked) and returns.
        vTaskStartScheduler();
    }
    return ok;
}

void sil_fw_advance_tick(void)
{
    // Hardware time first, so tasks waking this tick read a fresh timebase.
    HW_TIM_advanceTime(1000000U / configTICK_RATE_HZ);
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
    initSuccess &= prvAppInit();
    if (!initSuccess)
    {
        Error_Handler();
    }

    // Spawn the periodic tasks and hand control to the scheduler.
    // vTaskStartScheduler() does not return.
    if (!prvCreateTasks())
    {
        Error_Handler();
    }

    vTaskStartScheduler();
    return 0;
}
#endif

#if (BUILD_TARGET == BUILD_TARGET_SIM)
// Standalone SIL smoke driver over the control ABI (sil_fw.h). Unused inside the
// DLL (the Rust framework drives the same three calls and owns pacing); this
// loop is a temporary in-process stand-in. It runs the SAME bring-up path as
// sil_fw_start — no duplicated init — and watches the ADC sim ramp advance
// tick-over-tick to confirm the real tasks run.
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
        printf("tick %2u  adc[ch%u,in%u]=%u\n",
               tick, (unsigned)watchCh, watchIn, counts);
    }

    sil_fw_shutdown();
    return 0;
}
#endif
