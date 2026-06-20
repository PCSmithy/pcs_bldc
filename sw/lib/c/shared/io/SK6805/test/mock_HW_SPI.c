#include "HW_SPI.h"
#include "mock_HW_SPI.h"

#define MOCK_TXBUF_MAX  512U

static uint8_t mockTx[HW_SPI_CHANNEL_COUNT][MOCK_TXBUF_MAX];
static size_t  mockTxLen[HW_SPI_CHANNEL_COUNT];
static bool    mockTransferOk[HW_SPI_CHANNEL_COUNT];

void mock_HW_SPI_reset(void)
{
    for (size_t ch = 0U; ch < HW_SPI_CHANNEL_COUNT; ch++)
    {
        mockTxLen[ch]      = 0U;
        mockTransferOk[ch] = true;
        for (size_t i = 0U; i < MOCK_TXBUF_MAX; i++) { mockTx[ch][i] = 0U; }
    }
}

void mock_HW_SPI_setTransferOk(HW_SPI_channel_E channel, bool ok)
{
    if (channel < HW_SPI_CHANNEL_COUNT) { mockTransferOk[channel] = ok; }
}

const uint8_t * mock_HW_SPI_txBuf(HW_SPI_channel_E channel)
{
    return (channel < HW_SPI_CHANNEL_COUNT) ? mockTx[channel] : NULL;
}

size_t mock_HW_SPI_txLen(HW_SPI_channel_E channel)
{
    return (channel < HW_SPI_CHANNEL_COUNT) ? mockTxLen[channel] : 0U;
}

bool HW_SPI_transmit(HW_SPI_channel_E channel, uint8_t * txData, size_t length)
{
    bool ret = false;
    if ((channel < HW_SPI_CHANNEL_COUNT) && (txData != NULL) && (length <= MOCK_TXBUF_MAX))
    {
        for (size_t i = 0U; i < length; i++) { mockTx[channel][i] = txData[i]; }
        mockTxLen[channel] = length;
        ret = mockTransferOk[channel];
    }
    return ret;
}
