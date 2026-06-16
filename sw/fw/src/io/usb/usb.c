#include "usb.h"
#include "tusb.h"
#include "stm32g4xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
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
        if (tud_cdc_connected() && ((ticks % 10U) == 0U))
        {
            printf("pcs_bldc alive: %lu\r\n", (unsigned long)(ticks / 10U));
        }
    }
}

void USB_init(void)
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
    (void) xTaskCreate(usbDeviceTask, "usb", 512, NULL, configMAX_PRIORITIES - 2, NULL);
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
