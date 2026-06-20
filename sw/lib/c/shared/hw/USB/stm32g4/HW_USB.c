/* Includes */
#include "HW_USB.h"
#include "tusb.h"
#include "stm32g4xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"

/* Private Function Declarations */

static void HW_USB_serviceTask(void * params);

/* Private Function Definitions */

// USB device-service task: owns the TinyUSB device stack. It does nothing but
// initialise the stack and then service it forever — tud_task() blocks on the
// stack's event queue and, among other things, drains the CDC TX FIFO toward
// the host. This is the only task that calls tud_task(), keeping the stack
// single-threaded.
static void HW_USB_serviceTask(void * params)
{
    (void) params;
    tud_init(0);            // init device stack on rhport 0
    for (;;)
    {
        tud_task();         // block until a USB event, then service it
    }
}

/* Public Function Definitions */

// [impl->fw~hal_usb_001~1]
bool HW_USB_init(uint32_t taskPriority)
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

    // Surface a creation failure (e.g. heap exhaustion) rather than silently
    // leaving the stack uninitialised.
    return (xTaskCreate(HW_USB_serviceTask, "usbd", configMINIMAL_STACK_SIZE * 2U,
                        NULL, (UBaseType_t)taskPriority, NULL) == pdPASS);
}

// [impl->fw~hal_usb_002~1]
bool HW_USB_connected(void)
{
    return tud_cdc_connected();
}

// [impl->fw~hal_usb_003~1]
uint32_t HW_USB_write(const uint8_t * data, uint32_t len)
{
    return (uint32_t) tud_cdc_write(data, len);
}

void HW_USB_writeFlush(void)
{
    (void) tud_cdc_write_flush();
}

void HW_USB_serviceYield(void)
{
    // Yield a tick so the (higher-or-equal-priority) service task drains the
    // TX FIFO before the caller retries.
    vTaskDelay(1U);
}

// [impl->fw~hal_usb_004~1]
uint32_t HW_USB_available(void)
{
    return (uint32_t) tud_cdc_available();
}

uint32_t HW_USB_read(uint8_t * buffer, uint32_t len)
{
    return (uint32_t) tud_cdc_read(buffer, len);
}
