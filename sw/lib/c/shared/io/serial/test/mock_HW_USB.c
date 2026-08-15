/* Includes */
#include "mock_HW_USB.h"
#include "HW_USB.h"

#include <string.h>

/* Defines */
#define MOCK_HW_USB_BUF_LEN (256U)

/* Typedefs */
typedef struct
{
    bool     connected;
    bool     txAccepting;
    uint8_t  tx[MOCK_HW_USB_BUF_LEN];
    uint32_t txLen;
    uint8_t  rx[MOCK_HW_USB_BUF_LEN];
    uint32_t rxLen;
    uint32_t rxHead;
} mock_HW_USB_data_S;

/* Private Data Definitions */
static mock_HW_USB_data_S mock_HW_USB_data;
static mock_HW_USB_data_S * const data = &mock_HW_USB_data;

/* Mock Controls */

void mock_HW_USB_reset(void)
{
    *data = (mock_HW_USB_data_S){ 0 };
    data->txAccepting = true;
}

void mock_HW_USB_setConnected(bool connected)
{
    data->connected = connected;
}

void mock_HW_USB_setTxAccepting(bool accepting)
{
    data->txAccepting = accepting;
}

uint32_t mock_HW_USB_txLen(void)
{
    return data->txLen;
}

uint32_t mock_HW_USB_readTx(uint8_t * out, uint32_t len)
{
    const uint32_t count = (len < data->txLen) ? len : data->txLen;
    memcpy(out, data->tx, count);
    return count;
}

void mock_HW_USB_injectRx(const uint8_t * bytes, uint32_t len)
{
    const uint32_t space = MOCK_HW_USB_BUF_LEN - data->rxLen;
    const uint32_t count = (len < space) ? len : space;
    memcpy(&data->rx[data->rxLen], bytes, count);
    data->rxLen += count;
}

/* HW_USB API */

bool HW_USB_init(void)
{
    return true;
}

void HW_USB_run(void)
{
}

bool HW_USB_connected(void)
{
    return data->connected;
}

uint32_t HW_USB_write(const uint8_t * bytes, uint32_t len)
{
    uint32_t accepted = 0U;
    if (data->txAccepting)
    {
        const uint32_t space = MOCK_HW_USB_BUF_LEN - data->txLen;
        accepted = (len < space) ? len : space;
        memcpy(&data->tx[data->txLen], bytes, accepted);
        data->txLen += accepted;
    }
    return accepted;
}

void HW_USB_writeFlush(void)
{
}

void HW_USB_serviceYield(void)
{
}

uint32_t HW_USB_available(void)
{
    return data->rxLen - data->rxHead;
}

uint32_t HW_USB_read(uint8_t * buffer, uint32_t len)
{
    const uint32_t avail = data->rxLen - data->rxHead;
    const uint32_t count = (len < avail) ? len : avail;
    memcpy(buffer, &data->rx[data->rxHead], count);
    data->rxHead += count;
    return count;
}
