#include "usb.h"
#include "tusb.h"
#include "stm32g4xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include "IO_AS5048.h"
#include "lib_utils.h"
#include <stdio.h>

// TEMPORARY bring-up: this task inits the TinyUSB device stack, services it
// forever, and periodically prints a liveness counter over CDC once a terminal
// is connected. Throwaway smoke test for USB enumeration / serial — replace
// with real protocol handling once the USB CDC framing lands.
static void usbDeviceTask(void * params)
{
    (void) params;
    tud_init(0);            // init device stack on rhport 0
    // Unbuffered stdout so each printf reaches CDC immediately — newlib would
    // otherwise block-buffer (stdout isn't a tty) and not flush on '\n'.
    setvbuf(stdout, NULL, _IONBF, 0);
    uint32_t ticks = 0;
    for (;;)
    {
        tud_task_ext(100, false);   // service USB, ~100ms timeout so the loop also prints
        ticks++;
        // M2 bring-up: read encoder 1 and print its angle ~every 200ms. The
        // read happens on this task so all CDC printf stays single-threaded.
        if (tud_cdc_connected() && ((ticks % 2U) == 0U))
        {
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

            printf("Motor: raw=%5u  angle=%3lu.%02lu deg\r\tDial: raw=%5u  angle=%3lu.%02lu deg\r\n",
                    (unsigned)motorAngleRaw,
                    (unsigned long)motorAngleScaled,
                    (unsigned long)motorAngleDecimalScaled,
                    (unsigned)dialAngleRaw,
                    (unsigned long)dialAngleScaled,
                    (unsigned long)dialAngleDecimalScaled);
        }
    }
}

bool USB_init(void)
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

    // USB needs a deeper stack than the heartbeat task; run it high-ish.
    // Surface a creation failure (e.g. heap exhaustion) to the caller rather
    // than silently leaving the device stack uninitialized.
    return (xTaskCreate(usbDeviceTask, "usb", 512, NULL, configMAX_PRIORITIES - 2, NULL) == pdPASS);
}

// Retarget printf to CDC. syscalls.c's weak _write already calls __io_putchar.
int __io_putchar(int ch)
{
    (void) tud_cdc_write_char((char) ch);
    if (ch == '\n')
    {
        (void) tud_cdc_write_flush();
    }
    return ch;
}
