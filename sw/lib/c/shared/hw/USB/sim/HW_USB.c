/* Includes */
#include "HW_USB.h"
#include "HW_USB_sim.h"
#include "SIL_irq.h"
#include "FreeRTOS.h"
#include "task.h"

/* Defines */

#define HW_USB_SIM_BUF  512U

// Cadence of the simulated USB device interrupt (sim us). One event per
// millisecond mirrors the SOF rate the real device stack services at.
#define HW_USB_SIM_IRQ_PERIOD_US  1000U
// Ordering only, no preemption. USB sits below a control-loop ISR.
#define HW_USB_SIM_IRQ_PRIORITY   8U

/* Private Data Definitions */

typedef struct
{
    bool connected;
    bool txAccepting;

    uint8_t  tx[HW_USB_SIM_BUF];
    uint32_t txLen;

    uint8_t  rx[HW_USB_SIM_BUF];
    uint32_t rxHead;
    uint32_t rxTail;

    // The task servicing HW_USB_run, latched on its first call so the simulated
    // interrupt knows whom to wake. NULL until task_usb first runs.
    TaskHandle_t serviceTask;
} HW_USB_sim_data_S;

static HW_USB_sim_data_S HW_USB_sim_data;
static HW_USB_sim_data_S * const data = &HW_USB_sim_data;

/* Public Function Definitions */

// The simulated USB device interrupt. The framework dispatches it on the sim
// grid inside the port's ISR bracket; like the real handler it only wakes the
// service task, which does the work.
void HW_USB_sim_irqHandler(void)
{
    BaseType_t higherPriorityTaskWoken = pdFALSE;
    if (data->serviceTask != NULL)
    {
        vTaskNotifyGiveFromISR(data->serviceTask, &higherPriorityTaskWoken);
    }
    portYIELD_FROM_ISR(higherPriorityTaskWoken);
}

// [impl->fw~hal_usb_001~1]
bool HW_USB_init(void)
{
    data->txAccepting = true;
    (void)SIL_irq_registerPeriodic(HW_USB_sim_irqHandler,
                                   HW_USB_SIM_IRQ_PERIOD_US,
                                   HW_USB_SIM_IRQ_PRIORITY);
    return true;
}

// The loopback sim has no device stack to pump; TX/RX are driven directly by the
// SIL hooks. task_usb calls this in a tight loop, so it must block each
// iteration — on the simulated USB interrupt, exactly as the real task blocks on
// the device stack's event queue — or it would starve every other task and
// prevent the quiescence the driver waits on.
void HW_USB_run(void)
{
    if (data->serviceTask == NULL)
    {
        data->serviceTask = xTaskGetCurrentTaskHandle();
    }

    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

// [impl->fw~hal_usb_002~1]
bool HW_USB_connected(void)
{
    return data->connected;
}

// [impl->fw~hal_usb_003~1]
uint32_t HW_USB_write(const uint8_t * data_in, uint32_t len)
{
    uint32_t n = 0U;
    if (data->txAccepting)
    {
        while ((n < len) && (data->txLen < HW_USB_SIM_BUF))
        {
            data->tx[data->txLen] = data_in[n];
            data->txLen++;
            n++;
        }
    }
    return n;
}

void HW_USB_writeFlush(void)
{
}

void HW_USB_serviceYield(void)
{
}

// [impl->fw~hal_usb_004~1]
uint32_t HW_USB_available(void)
{
    return (data->rxTail - data->rxHead);
}

uint32_t HW_USB_read(uint8_t * buffer, uint32_t len)
{
    uint32_t n = 0U;
    while ((n < len) && (data->rxHead < data->rxTail))
    {
        buffer[n] = data->rx[data->rxHead];
        data->rxHead++;
        n++;
    }
    return n;
}

/* SIL inspection / control */

void HW_USB_sim_reset(void)
{
    // Loopback state only: the service-task latch is seam wiring, not device
    // state, and outlives a reset.
    const TaskHandle_t serviceTask = data->serviceTask;
    *data = (HW_USB_sim_data_S){ 0 };
    data->txAccepting = true;
    data->serviceTask = serviceTask;
}

void HW_USB_sim_setConnected(bool connected)
{
    data->connected = connected;
}

void HW_USB_sim_setTxAccepting(bool accepting)
{
    data->txAccepting = accepting;
}

uint32_t HW_USB_sim_readTx(uint8_t * buffer, uint32_t len)
{
    uint32_t n = 0U;
    while ((n < len) && (n < data->txLen))
    {
        buffer[n] = data->tx[n];
        n++;
    }
    return n;
}

uint32_t HW_USB_sim_txLen(void)
{
    return data->txLen;
}

void HW_USB_sim_injectRx(const uint8_t * src, uint32_t len)
{
    for (uint32_t i = 0U; (i < len) && (data->rxTail < HW_USB_SIM_BUF); i++)
    {
        data->rx[data->rxTail] = src[i];
        data->rxTail++;
    }
}
