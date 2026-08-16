/* Includes */
#include "HW_USB.h"
#include "tusb.h"
#include "stm32g4xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"

/* Public Function Definitions */

// [impl->fw~hal_usb_001~1]
bool HW_USB_init(void)
{
    // Route the USB clock: PLL-Q = 48 MHz (matches ST usbd_conf.c MspInit).
    RCC_PeriphCLKInitTypeDef p = {0};
    p.PeriphClockSelection = RCC_PERIPHCLK_USB;
    p.UsbClockSelection = RCC_USBCLKSOURCE_PLL;
    (void) HAL_RCCEx_PeriphCLKConfig(&p);
    __HAL_RCC_USB_CLK_ENABLE();

    // Set the USB IRQ priority BEFORE initialising the stack. tud_init() enables
    // USB_LP_IRQn (via dcd_int_enable) and asserts the DP pull-up, after which the
    // host can begin enumerating and the interrupt can fire immediately — so it
    // must already sit at a FreeRTOS-safe priority (numerically >=
    // configMAX_SYSCALL_INTERRUPT_PRIORITY), or its first FromISR call trips
    // configASSERT in vPortValidateInterruptPriority. Priority 5 = FreeRTOS-safe,
    // matching our EXTI convention.
    HAL_NVIC_SetPriority(USB_LP_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(USB_LP_IRQn);

    // tud_init creates the event queue before it enables the interrupt, so the
    // queue exists by the time the first USB IRQ fires.
    return tud_init(0);
}

void HW_USB_run(void)
{
    // Service the device stack: block on the event queue until the USB ISR posts
    // an event, process it, and push queued CDC TX toward the host. This is the
    // body of a dedicated USB task — tud_task() runs its own internal service
    // loop and does not return under normal operation.
    tud_task();
}

// [impl->fw~hal_usb_002~1]
bool HW_USB_connected(void)
{
    return tud_cdc_connected();
}

// tud_cdc's ready/connected calls are internally gated on the device being
// configured, but its FIFO read/write paths are NOT: before the host
// configures the device the CDC endpoints are unopened (address 0), and a
// read/write can claim and queue transfers on endpoint 0 — corrupting the
// control transfers of enumeration itself. Guard every FIFO path on
// tud_ready().

// [impl->fw~hal_usb_003~1]
uint32_t HW_USB_write(const uint8_t * data, uint32_t len)
{
    uint32_t accepted = 0U;
    if (tud_ready())
    {
        accepted = (uint32_t) tud_cdc_write(data, len);
    }
    return accepted;
}

// [impl->fw~hal_usb_005~1]
uint32_t HW_USB_writeAvailable(void)
{
    uint32_t available = 0U;
    if (tud_ready())
    {
        available = (uint32_t) tud_cdc_write_available();
    }
    return available;
}

void HW_USB_writeFlush(void)
{
    if (tud_ready())
    {
        (void) tud_cdc_write_flush();
    }
}

void HW_USB_serviceYield(void)
{
    // Yield a tick so the 1 ms task services the USB stack and drains the TX
    // FIFO before the caller retries.
    vTaskDelay(1U);
}

// [impl->fw~hal_usb_004~1]
uint32_t HW_USB_available(void)
{
    uint32_t count = 0U;
    if (tud_ready())
    {
        count = (uint32_t) tud_cdc_available();
    }
    return count;
}

uint32_t HW_USB_read(uint8_t * buffer, uint32_t len)
{
    uint32_t count = 0U;
    if (tud_ready())
    {
        count = (uint32_t) tud_cdc_read(buffer, len);
    }
    return count;
}
