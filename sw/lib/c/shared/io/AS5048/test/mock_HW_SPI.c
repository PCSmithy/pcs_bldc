#include "HW_SPI.h"
#include "mock_HW_SPI.h"

static uint16_t mockResponse[HW_SPI_CHANNEL_COUNT];
static bool     mockTransferOk[HW_SPI_CHANNEL_COUNT];
static uint16_t mockLastCommand[HW_SPI_CHANNEL_COUNT];

void mock_HW_SPI_reset(void)
{
    for (size_t ch = 0U; ch < HW_SPI_CHANNEL_COUNT; ch++)
    {
        mockResponse[ch]    = 0U;
        mockTransferOk[ch]  = true;
        mockLastCommand[ch] = 0U;
    }
}

void mock_HW_SPI_setResponse(HW_SPI_channel_E channel, uint16_t frame)
{
    if (channel < HW_SPI_CHANNEL_COUNT) { mockResponse[channel] = frame; }
}

void mock_HW_SPI_setTransferOk(HW_SPI_channel_E channel, bool ok)
{
    if (channel < HW_SPI_CHANNEL_COUNT) { mockTransferOk[channel] = ok; }
}

uint16_t mock_HW_SPI_lastCommand(HW_SPI_channel_E channel)
{
    return (channel < HW_SPI_CHANNEL_COUNT) ? mockLastCommand[channel] : 0U;
}

bool HW_SPI_transmitReceive(HW_SPI_channel_E channel, uint8_t * txData, uint8_t * rxData, size_t length)
{
    bool ret = false;
    if ((channel < HW_SPI_CHANNEL_COUNT) && (length == 2U))
    {
        if (txData != NULL)
        {
            mockLastCommand[channel] = (uint16_t)(((uint16_t)txData[0] << 8U) | (uint16_t)txData[1]);
        }
        if (rxData != NULL)
        {
            rxData[0] = (uint8_t)(mockResponse[channel] >> 8U);
            rxData[1] = (uint8_t)(mockResponse[channel] & 0xFFU);
        }
        ret = mockTransferOk[channel];
    }
    return ret;
}
