#include "usb.h"
#include "tusb.h"
#include "stm32g4xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include "IO_AS5048.h"
#include "HW_ADC.h"
#include "lib_utils.h"
#include <stdio.h>

// Teleplot wire format (beacon): one packet per "name:timestamp_ms:value[§unit]",
// packets separated by ';' or '\n'. The unit separator is the section sign
// U+00A7, encoded UTF-8 as 0xC2 0xA7. Kept as its own string literal so the
// adjacent unit text isn't swallowed into the \x hex escape (a hex escape eats
// every following hex digit, and 'd'/'e' are hex digits) — adjacent string
// literals concatenate at compile time.
#define TP_UNIT "\xC2\xA7"

// Telemetry emit period (ms).
#define TELEMETRY_PERIOD_MS 25

// USB device service task: owns the TinyUSB device stack. It does nothing but
// initialise the stack and then service it forever — tud_task() blocks on the
// stack's event queue and, among other things, drains the CDC TX FIFO toward
// the host. This is the *only* task that calls tud_task(), keeping the stack
// single-threaded.
static void usbServiceTask(void * params)
{
    (void) params;
    tud_init(0);            // init device stack on rhport 0
    for (;;)
    {
        tud_task();         // block until a USB event, then service it
    }
}

// Telemetry producer task: on a fixed cadence, reads the current sensor values
// and streams them over CDC in beacon's Teleplot text format for live plotting.
// It is the only writer into the CDC FIFO (the service task is the only reader),
// so CDC output stays single-producer / single-consumer.
static void usbTelemetryTask(void * params)
{
    (void) params;

    // Unbuffered stdout so each printf reaches CDC immediately — newlib would
    // otherwise block-buffer (stdout isn't a tty) and not flush on '\n'.
    setvbuf(stdout, NULL, _IONBF, 0);

    // vTaskDelayUntil anchors to the previous wake time, giving a drift-free
    // period regardless of how long the read+print work takes.
    TickType_t lastWake = xTaskGetTickCount();
    for (;;)
    {
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(TELEMETRY_PERIOD_MS));

        // Nothing is listening until a host opens the port — skip the work.
        if (!tud_cdc_connected())
        {
            continue;
        }

        // Source timestamp (ms) shared by every signal in this pass. FreeRTOS
        // owns SysTick here (FreeRTOSConfig.h maps xPortSysTickHandler ->
        // SysTick_Handler), so HAL's uwTick never advances and HAL_GetTick()
        // stays 0 — use the RTOS tick instead. configTICK_RATE_HZ is 1000, so
        // one tick == 1 ms. Beacon unwraps the uint32 wrap (~49.7 days).
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

        // Motor + dial: angle in degrees (scale 100 -> 2 dp) plus raw count.
        printf("motor_angle:%lu:%lu.%02lu" TP_UNIT "deg;"
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

        // ADC bring-up: the two enabled regular inputs (ADC1_IN6,
        // ADC2_IN11) as raw counts + volts, with each channel's
        // last-pass conversion status.
        uint32_t  adc1Count = 0U;
        float32_t adc1Volts = 0.0f;
        (void)HW_ADC_getCount(HW_ADC_CHANNEL_1, 6U, &adc1Count);
        (void)HW_ADC_getVolts(HW_ADC_CHANNEL_1, 6U, &adc1Volts);

        uint32_t  adc2Count = 0U;
        float32_t adc2Volts = 0.0f;
        (void)HW_ADC_getCount(HW_ADC_CHANNEL_2, 11U, &adc2Count);
        (void)HW_ADC_getVolts(HW_ADC_CHANNEL_2, 11U, &adc2Volts);

        uint32_t adc1Whole = 0U;
        uint32_t adc1Frac  = 0U;
        floatToFixed(adc1Volts, 1000U, &adc1Whole, &adc1Frac);
        uint32_t adc2Whole = 0U;
        uint32_t adc2Frac  = 0U;
        floatToFixed(adc2Volts, 1000U, &adc2Whole, &adc2Frac);

        HW_ADC_conversionStatus_E adc1Status = HW_ADC_CONVERSION_STATUS_IDLE;
        HW_ADC_conversionStatus_E adc2Status = HW_ADC_CONVERSION_STATUS_IDLE;
        (void)HW_ADC_getStatus(HW_ADC_CHANNEL_1, &adc1Status);
        (void)HW_ADC_getStatus(HW_ADC_CHANNEL_2, &adc2Status);

        // ADC1_IN6 and ADC2_IN11: raw count, volts (scale 1000 -> 3 dp),
        // and last-pass conversion status (enum emitted as a plain number).
        printf("adc1_cnt:%lu:%lu;"
               "adc1_v:%lu:%lu.%03lu" TP_UNIT "V;"
               "adc1_status:%lu:%u\n",
                (unsigned long)nowMs,
                (unsigned long)adc1Count,
                (unsigned long)nowMs,
                (unsigned long)adc1Whole,
                (unsigned long)adc1Frac,
                (unsigned long)nowMs,
                (unsigned)adc1Status);
        printf("adc2_cnt:%lu:%lu;"
               "adc2_v:%lu:%lu.%03lu" TP_UNIT "V;"
               "adc2_status:%lu:%u\n",
                (unsigned long)nowMs,
                (unsigned long)adc2Count,
                (unsigned long)nowMs,
                (unsigned long)adc2Whole,
                (unsigned long)adc2Frac,
                (unsigned long)nowMs,
                (unsigned)adc2Status);
    }
}

bool USB_init(uint32_t taskPriority)
{
    // Route the USB clock: PLL-Q = 48 MHz (matches ST usbd_conf.c MspInit).
    RCC_PeriphCLKInitTypeDef p = {0};
    p.PeriphClockSelection = RCC_PERIPHCLK_USB;
    p.UsbClockSelection = RCC_USBCLKSOURCE_PLL;
    (void) HAL_RCCEx_PeriphCLKConfig(&p);
    __HAL_RCC_USB_CLK_ENABLE();

    // Priority 5 = FreeRTOS-safe (numerically >= configMAX_SYSCALL_INTERRUPT_PRIORITY),
    // matching our EXTI convention.
    HAL_NVIC_SetPriority(USB_LP_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(USB_LP_IRQn);

    // Two tasks: the device service task runs at the caller's USB priority so
    // stack servicing (enumeration, control transfers, FIFO drain) stays
    // responsive; the telemetry producer runs one step lower since its cadence
    // is soft real-time. When the producer hits a full FIFO it yields, so the
    // service task drains regardless of which is higher — but keeping the
    // service task ahead minimises latency. Surface a creation failure (e.g.
    // heap exhaustion) rather than silently leaving the stack half-initialised.
    const UBaseType_t servicePrio = (UBaseType_t)taskPriority;
    const UBaseType_t telemetryPrio = (taskPriority > (uint32_t)tskIDLE_PRIORITY + 1U)
                                    ? (UBaseType_t)(taskPriority - 1U)
                                    : (UBaseType_t)taskPriority;

    bool ok = (xTaskCreate(usbServiceTask, "usbd", configMINIMAL_STACK_SIZE * 2U,
                           NULL, servicePrio, NULL) == pdPASS);
    ok = ok && (xTaskCreate(usbTelemetryTask, "telem", 512,
                            NULL, telemetryPrio, NULL) == pdPASS);
    return ok;
}

// Retarget printf to CDC. syscalls.c's weak _write already calls __io_putchar.
int __io_putchar(int ch)
{
    // Apply backpressure: tud_cdc_write_char returns 0 when the TX FIFO is full.
    // Silently dropping the character (the old behaviour) truncated lines and
    // jammed samples together once we wrote faster than the host drained. The
    // USB service task owns tud_task(), so we must NOT pump the stack from here;
    // instead flush what we have and yield, letting that task drain the FIFO,
    // then retry. Bounded so a stalled/disconnected host can't wedge the task.
    uint32_t spins = 0U;
    while (tud_cdc_connected() && (tud_cdc_write_char((char) ch) == 0U))
    {
        (void) tud_cdc_write_flush();   // hand buffered data to the device stack
        vTaskDelay(1U);                 // yield so usbServiceTask can drain it
        if (++spins > 1000U)
        {
            break;
        }
    }
    if (ch == '\n')
    {
        (void) tud_cdc_write_flush();
    }
    return ch;
}
