#include "lib_build.h"
#include "HW_systemClock.h"
#include "HW_GPIO.h"
#include "HW_OPAMP.h"
#include "HW_ADC.h"
#include "HW_SPI.h"
#include "HW_TIM.h"
#include "HW_DMA.h"

#if (BUILD_TARGET == BUILD_TARGET_STM32G4)
  #include "stm32g4xx_hal.h"  // HAL_Init
  #include "FreeRTOS.h"
  #include "task.h"
  #include <stdio.h>
  #include <math.h>           // fabsf, for signed telemetry formatting
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
extern const HW_OPAMP_config_S HW_OPAMP_config;
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

#if (BUILD_TARGET == BUILD_TARGET_STM32G4)
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
#define TELEMETRY_PERIOD_MS 2

// One window's Teleplot packets are formatted into a buffer this size and pushed
// with a single IO_serial_write, so the CDC FIFO is flushed once per window
// rather than once per byte (the per-byte path is far too slow to keep up).
// Sized to hold a full window's worst case (~600 B with the ADC engineering
// signals below, incl. the three phase-voltage sense channels); kept in step
// with CFG_TUD_CDC_TX_BUFSIZE so the batched write drains without backpressure.
#define TELEMETRY_TX_BUF_BYTES 1024U

// --- ADC engineering-unit scaling (bring-up scaffolding) -------------------
// Maps raw pin volts (HW_ADC_getVolts) to amps/volts using the board's sense
// front end. Acknowledged scaffolding — revisit once the analog path is
// characterized. Phase current: INA240A3 (100 V/V) across a 1 mOhm shunt ->
// 0.1 V/A, biased to a VREF/2 = 1.65 V zero-current midpoint: i = (v - 1.65)/0.1.
#define ADC_PHASE_I_OFFSET_V   (1.65f)
#define ADC_PHASE_I_V_PER_A    (0.1f)
// VBUS current: INA180A2 (50 V/V) across a 12 mOhm shunt -> 0.6 V/A, ground
// referenced: i = v / 0.6.
#define ADC_VBUS_I_V_PER_A     (0.6f)
// VBUS voltage: resistive divider, 0.15 V/V: v = v_adc / 0.15.
#define ADC_VBUS_V_RATIO       (0.15f)
// Phase voltage sense: each phase node feeds a 274.0k/22.1k resistive
// divider (22.1/296.1 V/V) into an OPAMP PGA at x2 whose output the ADC
// samples. Recover the phase voltage: v_phase = v_adc / gain / ratio
// (~x6.699 overall).
#define ADC_PHASE_V_OPAMP_GAIN (2.0f)
#define ADC_PHASE_V_DIV_RATIO  (22.1f / 296.1f)
// 5V0 / 3V3 rails: half-divider (two equal resistors), 0.5 V/V: v = v_adc / 0.5.
#define ADC_RAIL_V_RATIO       (0.5f)

// floatToFixed casts to uint32_t, so it underflows on negative inputs. Phase
// currents sit near zero and swing negative, so split the sign off and
// fixed-point the magnitude; the caller prints the returned "" / "-" ahead of
// the "whole.frac" pair so e.g. -0.3 renders as "-0.300", not a garbage whole.
static const char * telemetrySignedFixed(float32_t value, uint32_t scale,
                                         uint32_t * const whole, uint32_t * const frac)
{
    floatToFixed(fabsf(value), scale, whole, frac);
    return ((value < 0.0f)) ? "-" : "";
}

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

        // Source timestamp (ms) shared by every signal in this pass. The RTOS
        // tick is the natural in-task time base (1 ms period).
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

        // ADC engineering-unit signals. getVolts returns each pin's voltage;
        // the scaling constants above turn it into amps/volts. Phase currents
        // are bipolar (swing negative), so telemetrySignedFixed splits the sign.
        float32_t phaseU_v = 0.0f;
        float32_t phaseV_v = 0.0f;
        float32_t phaseW_v = 0.0f;
        float32_t vbusI_v  = 0.0f;
        float32_t vbusV_v  = 0.0f;
        float32_t rail5_v  = 0.0f;
        float32_t rail3_v  = 0.0f;
        (void)HW_ADC_getVolts(HW_ADC_CHANNEL_1, 6U, &phaseU_v);
        (void)HW_ADC_getVolts(HW_ADC_CHANNEL_2, 7U, &phaseV_v);
        (void)HW_ADC_getVolts(HW_ADC_CHANNEL_1, 8U, &phaseW_v);
        (void)HW_ADC_getVolts(HW_ADC_CHANNEL_2, 11U, &vbusI_v);
        (void)HW_ADC_getVolts(HW_ADC_CHANNEL_1, 12U, &vbusV_v);
        (void)HW_ADC_getVolts(HW_ADC_CHANNEL_1, 1U, &rail5_v);
        (void)HW_ADC_getVolts(HW_ADC_CHANNEL_2, 9U, &rail3_v);

        const float32_t phaseU_i = (phaseU_v - ADC_PHASE_I_OFFSET_V) / ADC_PHASE_I_V_PER_A;
        const float32_t phaseV_i = (phaseV_v - ADC_PHASE_I_OFFSET_V) / ADC_PHASE_I_V_PER_A;
        const float32_t phaseW_i = (phaseW_v - ADC_PHASE_I_OFFSET_V) / ADC_PHASE_I_V_PER_A;
        const float32_t vbus_i   = vbusI_v / ADC_VBUS_I_V_PER_A;
        const float32_t vbus_v   = vbusV_v / ADC_VBUS_V_RATIO;
        const float32_t rail5v0  = rail5_v / ADC_RAIL_V_RATIO;
        const float32_t rail3v3  = rail3_v / ADC_RAIL_V_RATIO;

        uint32_t phaseUWhole = 0U;
        uint32_t phaseUFrac  = 0U;
        const char * const phaseUSign = telemetrySignedFixed(phaseU_i, 1000U, &phaseUWhole, &phaseUFrac);
        uint32_t phaseVWhole = 0U;
        uint32_t phaseVFrac  = 0U;
        const char * const phaseVSign = telemetrySignedFixed(phaseV_i, 1000U, &phaseVWhole, &phaseVFrac);
        uint32_t phaseWWhole = 0U;
        uint32_t phaseWFrac  = 0U;
        const char * const phaseWSign = telemetrySignedFixed(phaseW_i, 1000U, &phaseWWhole, &phaseWFrac);
        uint32_t vbusIWhole = 0U;
        uint32_t vbusIFrac  = 0U;
        const char * const vbusISign = telemetrySignedFixed(vbus_i, 1000U, &vbusIWhole, &vbusIFrac);
        uint32_t vbusVWhole = 0U;
        uint32_t vbusVFrac  = 0U;
        const char * const vbusVSign = telemetrySignedFixed(vbus_v, 1000U, &vbusVWhole, &vbusVFrac);
        uint32_t rail5Whole = 0U;
        uint32_t rail5Frac  = 0U;
        const char * const rail5Sign = telemetrySignedFixed(rail5v0, 1000U, &rail5Whole, &rail5Frac);
        uint32_t rail3Whole = 0U;
        uint32_t rail3Frac  = 0U;
        const char * const rail3Sign = telemetrySignedFixed(rail3v3, 1000U, &rail3Whole, &rail3Frac);

        HW_ADC_conversionStatus_E adc1Status = HW_ADC_CONVERSION_STATUS_IDLE;
        HW_ADC_conversionStatus_E adc2Status = HW_ADC_CONVERSION_STATUS_IDLE;
        (void)HW_ADC_getStatus(HW_ADC_CHANNEL_1, &adc1Status);
        (void)HW_ADC_getStatus(HW_ADC_CHANNEL_2, &adc2Status);

        n = snprintf(&txBuf[off], sizeof(txBuf) - (size_t)off,
                     "phase_u_i:%lu:%s%lu.%03lu" TP_UNIT "A;"
                     "phase_v_i:%lu:%s%lu.%03lu" TP_UNIT "A;"
                     "phase_w_i:%lu:%s%lu.%03lu" TP_UNIT "A\n",
                     (unsigned long)nowMs, phaseUSign,
                     (unsigned long)phaseUWhole, (unsigned long)phaseUFrac,
                     (unsigned long)nowMs, phaseVSign,
                     (unsigned long)phaseVWhole, (unsigned long)phaseVFrac,
                     (unsigned long)nowMs, phaseWSign,
                     (unsigned long)phaseWWhole, (unsigned long)phaseWFrac);
        if ((n > 0) && ((size_t)n < (sizeof(txBuf) - (size_t)off))) { off += n; }

        // Phase terminal voltages via the OPAMP-buffered divider (IN13 on
        // ADC1, IN16/IN18 on ADC2). Non-negative, but formatted through the
        // signed helper to keep the fixed-point handling uniform.
        float32_t phaseUsense_v = 0.0f;
        float32_t phaseVsense_v = 0.0f;
        float32_t phaseWsense_v = 0.0f;
        (void)HW_ADC_getVolts(HW_ADC_CHANNEL_1, 13U, &phaseUsense_v);
        (void)HW_ADC_getVolts(HW_ADC_CHANNEL_2, 16U, &phaseVsense_v);
        (void)HW_ADC_getVolts(HW_ADC_CHANNEL_2, 18U, &phaseWsense_v);

        const float32_t phaseU_v_out = phaseUsense_v / ADC_PHASE_V_OPAMP_GAIN / ADC_PHASE_V_DIV_RATIO;
        const float32_t phaseV_v_out = phaseVsense_v / ADC_PHASE_V_OPAMP_GAIN / ADC_PHASE_V_DIV_RATIO;
        const float32_t phaseW_v_out = phaseWsense_v / ADC_PHASE_V_OPAMP_GAIN / ADC_PHASE_V_DIV_RATIO;

        uint32_t phaseUvWhole = 0U;
        uint32_t phaseUvFrac  = 0U;
        const char * const phaseUvSign = telemetrySignedFixed(phaseU_v_out, 1000U, &phaseUvWhole, &phaseUvFrac);
        uint32_t phaseVvWhole = 0U;
        uint32_t phaseVvFrac  = 0U;
        const char * const phaseVvSign = telemetrySignedFixed(phaseV_v_out, 1000U, &phaseVvWhole, &phaseVvFrac);
        uint32_t phaseWvWhole = 0U;
        uint32_t phaseWvFrac  = 0U;
        const char * const phaseWvSign = telemetrySignedFixed(phaseW_v_out, 1000U, &phaseWvWhole, &phaseWvFrac);

        n = snprintf(&txBuf[off], sizeof(txBuf) - (size_t)off,
                     "phase_u_v:%lu:%s%lu.%03lu" TP_UNIT "V;"
                     "phase_v_v:%lu:%s%lu.%03lu" TP_UNIT "V;"
                     "phase_w_v:%lu:%s%lu.%03lu" TP_UNIT "V\n",
                     (unsigned long)nowMs, phaseUvSign,
                     (unsigned long)phaseUvWhole, (unsigned long)phaseUvFrac,
                     (unsigned long)nowMs, phaseVvSign,
                     (unsigned long)phaseVvWhole, (unsigned long)phaseVvFrac,
                     (unsigned long)nowMs, phaseWvSign,
                     (unsigned long)phaseWvWhole, (unsigned long)phaseWvFrac);
        if ((n > 0) && ((size_t)n < (sizeof(txBuf) - (size_t)off))) { off += n; }

        n = snprintf(&txBuf[off], sizeof(txBuf) - (size_t)off,
                     "vbus_i:%lu:%s%lu.%03lu" TP_UNIT "A;"
                     "vbus_v:%lu:%s%lu.%03lu" TP_UNIT "V;"
                     "rail_5v0:%lu:%s%lu.%03lu" TP_UNIT "V;"
                     "rail_3v3:%lu:%s%lu.%03lu" TP_UNIT "V\n",
                     (unsigned long)nowMs, vbusISign,
                     (unsigned long)vbusIWhole, (unsigned long)vbusIFrac,
                     (unsigned long)nowMs, vbusVSign,
                     (unsigned long)vbusVWhole, (unsigned long)vbusVFrac,
                     (unsigned long)nowMs, rail5Sign,
                     (unsigned long)rail5Whole, (unsigned long)rail5Frac,
                     (unsigned long)nowMs, rail3Sign,
                     (unsigned long)rail3Whole, (unsigned long)rail3Frac);
        if ((n > 0) && ((size_t)n < (sizeof(txBuf) - (size_t)off))) { off += n; }

        // n = snprintf(&txBuf[off], sizeof(txBuf) - (size_t)off,
        //              "adc1_status:%lu:%u;"
        //              "adc2_status:%lu:%u\n",
        //              (unsigned long)nowMs, (unsigned)adc1Status,
        //              (unsigned long)nowMs, (unsigned)adc2Status);
        // if ((n > 0) && ((size_t)n < (sizeof(txBuf) - (size_t)off))) { off += n; }

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

        // One write per window: IO_serial_write batches the whole buffer into the
        // CDC FIFO in one call and flushes once.
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
    ok &= HW_OPAMP_init(&HW_OPAMP_config);   // before ADC: op-amps must be calibrated and running before the ADC samples their internal outputs
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
