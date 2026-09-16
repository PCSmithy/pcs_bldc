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
// Per-task worst-case body duration in microseconds (read/reset via
// profileTakeMaxUs). task_usb blocks on its event queue, so it is not profiled.
typedef enum
{
    PROFILE_TASK_1MS,
    PROFILE_TASK_10MS,
    PROFILE_TASK_200MS,
    PROFILE_TASK_SERVER,
    PROFILE_TASK_COUNT,
} profileTask_E;

static volatile uint32_t profileMaxUs[PROFILE_TASK_COUNT];

#if (BUILD_TARGET == BUILD_TARGET_SIM)
// Sim-only trace window the SIL scenarios watch (app_server_config.c owns it).
extern uint32_t app_server_simTraceWindow32[];
#endif

// --- Bridge cycle probe (fw~mc_018) ----------------------------------------
// Microsecond maxima from cycle-callback entry to the end of the commutation
// step and to callback exit; the host clears either by writing zero to it.
// [impl->fw~mc_018~1]
static volatile uint32_t main_cycleProbe_stepMax_us;
static volatile uint32_t main_cycleProbe_callbackMax_us;

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

/* Private Function Declarations */

// Init helpers shared by both targets' entry paths; main.c stays the only
// caller of Error_Handler.
static bool main_private_hwInit(void);
static bool main_private_appInit(void);
static bool main_private_createTasks(void);
static void main_private_bridgeCycle(IO_bridge_channel_E channel, void * context);

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
        // Sim trace window word [0]: the SIL trace scenarios' 1 kHz signal.
        app_server_simTraceWindow32[0]++;
#endif

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


// Protocol server: prioritized below the periodic control tasks, so serving
// the host never delays sampling or commutation.
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

// The PWM-synchronous cycle, entered from the bridge's per-cycle callback in
// injected-completion ISR context: no FreeRTOS call, no lib_timer, no printf.
// [impl->fw~mc_018~1]
static void main_private_bridgeCycle(IO_bridge_channel_E channel, void * context)
{
    (void)channel;
    (void)context;
    uint32_t entry_us = 0U;
    (void)HW_TIM_getCounter(IO_bridge_config.timeBasePeripheral, &entry_us);

    // --- commutation step (fw~mc_015): the active method's step lands here ---

    uint32_t stepEnd_us = 0U;
    (void)HW_TIM_getCounter(IO_bridge_config.timeBasePeripheral, &stepEnd_us);
    const uint32_t stepDuration_us = stepEnd_us - entry_us;
    if (stepDuration_us > main_cycleProbe_stepMax_us)
    {
        main_cycleProbe_stepMax_us = stepDuration_us;
    }

#if (BUILD_TARGET == BUILD_TARGET_SIM)
    // Sim trace window word [1]: the SIL trace scenarios' per-cycle signal,
    // word [2] its complement - a lockstep pair the coherence test checks.
    app_server_simTraceWindow32[1]++;
    app_server_simTraceWindow32[2] = ~app_server_simTraceWindow32[1];
#endif
    app_server_sampleCycle();   // capture trace watches after the step (fw~conn_trace_004)

    uint32_t exit_us = 0U;
    (void)HW_TIM_getCounter(IO_bridge_config.timeBasePeripheral, &exit_us);
    const uint32_t callbackDuration_us = exit_us - entry_us;
    if (callbackDuration_us > main_cycleProbe_callbackMax_us)
    {
        main_cycleProbe_callbackMax_us = callbackDuration_us;
    }
}

// HW-layer init, shared by both targets' entry paths.
static bool main_private_hwInit(void)
{
    bool ok = true;
    ok &= HW_systemClock_init(&HW_systemClock_config);
    ok &= HW_GPIO_init(&HW_GPIO_config);
    ok &= HW_OPAMP_init(&HW_OPAMP_config);   // before ADC: op-amps must be calibrated and running before the ADC samples their internal outputs
    ok &= HW_DMA_init(&HW_DMA_config);   // must run before SPI and ADC init()
    ok &= HW_ADC_init(&HW_ADC_config);
    ok &= HW_SPI_init(&HW_SPI_config);
    ok &= HW_I2C_init(&HW_I2C_config);
    ok &= HW_TIM_init(&HW_TIM_config);
    return ok;
}

// IO/dev/app-layer init, shared by both targets' entry paths. Aggregates each
// module's bool the same way main_private_hwInit does.
static bool main_private_appInit(void)
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
    // Last: the callback's first firing must find every module it drives up.
    ok &= IO_bridge_registerCycleCallback(IO_BRIDGE_CHANNEL_MOTOR, main_private_bridgeCycle, NULL);
    return ok;
}

// Spawn the periodic tasks. Same names/priorities/stacks on both targets; each
// allocates from the FreeRTOS heap, so a failure here (e.g. heap exhaustion) is
// surfaced to the caller to halt loudly rather than silently drop a task.
static bool main_private_createTasks(void)
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
extern void vPortYieldToScheduler(void);
extern BaseType_t xSilDispatchIsr(void (*pxHandler)(void));

// Quiescence handoff: when every task is blocked the idle task runs and hands
// control back to the driver (framework) fiber.
void vApplicationIdleHook(void)
{
    vPortYieldToScheduler();
}

// --- SIL control ABI (sil_fw.h) --------------------------------------------
// The framework drives these; pacing (fast vs realtime) is the driver's choice.
// The bring-up path is identical to the embedded main() below (minus HAL_Init):
// the SAME HW/app init and the SAME four tasks. The fiber port runs the
// scheduler to first quiescence and returns.

void sil_fw_setHooks(const SIL_ports_hooks_S * const hooks)
{
    SIL_ports_setHooks(hooks);
}

void sil_fw_setIrqHooks(const SIL_irq_hooks_S * const hooks)
{
    SIL_irq_setHooks(hooks);
}

bool sil_fw_start(void)
{
    bool ok = main_private_hwInit();
    ok = ok && main_private_appInit();
    ok = ok && main_private_createTasks();
    if (ok)
    {
        // Fiber port: runs to first quiescence (all tasks blocked) and returns.
        vTaskStartScheduler();
    }
    return ok;
}

void sil_fw_advance_time(uint32_t elapsed_us)
{
    // Called every engine step BEFORE any interrupt is dispatched, so a handler
    // (the kernel tick included) reads the timebase of the step it runs in.
    HW_TIM_advanceTime(elapsed_us);
}

bool sil_fw_dispatch_isr(SIL_irq_handler_F handler)
{
    return (xSilDispatchIsr(handler) != pdFALSE);
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

    bool initSuccess = main_private_hwInit();
    initSuccess &= main_private_appInit();
    if (!initSuccess)
    {
        Error_Handler();
    }

    // Spawn the periodic tasks and hand control to the scheduler.
    // vTaskStartScheduler() does not return.
    if (!main_private_createTasks())
    {
        Error_Handler();
    }

    vTaskStartScheduler();
    return 0;
}
#endif

#if (BUILD_TARGET == BUILD_TARGET_SIM)
// Standalone boot smoke check over the control ABI (sil_fw.h). Unused inside the
// DLL — the Rust framework is the driver. It boots and tears down, nothing more:
// with no framework hooks installed nothing registers an interrupt, so no kernel
// tick fires and the firmware cannot advance (docs/sil/sim-interrupts.md).
int main(void)
{
    if (!sil_fw_start())
    {
        return 1;
    }
    sil_fw_shutdown();
    return 0;
}
#endif
