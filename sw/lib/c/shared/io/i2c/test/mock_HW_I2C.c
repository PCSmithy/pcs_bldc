#include "HW_I2C.h"
#include "mock_HW_I2C.h"

static mock_HW_I2C_call_S mockLastCall;
static bool               mockTransferOk;
static uint8_t            mockResponse[MOCK_HW_I2C_MAX_BYTES];
static size_t             mockResponseLen;

void mock_HW_I2C_reset(void)
{
    mockLastCall = (mock_HW_I2C_call_S){ 0 };
    mockTransferOk = true;
    for (size_t i = 0U; i < MOCK_HW_I2C_MAX_BYTES; i++) { mockResponse[i] = 0U; }
    mockResponseLen = 0U;
}

void mock_HW_I2C_setTransferOk(bool ok)
{
    mockTransferOk = ok;
}

void mock_HW_I2C_setResponse(const uint8_t * bytes, size_t length)
{
    size_t n = (length < MOCK_HW_I2C_MAX_BYTES) ? length : MOCK_HW_I2C_MAX_BYTES;
    for (size_t i = 0U; i < n; i++) { mockResponse[i] = bytes[i]; }
    mockResponseLen = n;
}

const mock_HW_I2C_call_S * mock_HW_I2C_lastCall(void)
{
    return &mockLastCall;
}

bool HW_I2C_memRead(HW_I2C_bus_E bus, uint8_t devAddr7, uint16_t memAddr,
                    HW_I2C_memAddrSize_E memAddrSize, uint8_t * data, size_t length)
{
    mockLastCall.called      = true;
    mockLastCall.isWrite     = false;
    mockLastCall.bus         = bus;
    mockLastCall.devAddr7    = devAddr7;
    mockLastCall.memAddr     = memAddr;
    mockLastCall.memAddrSize = memAddrSize;
    mockLastCall.length      = length;

    if (data != NULL)
    {
        const size_t n = (length < mockResponseLen) ? length : mockResponseLen;
        for (size_t i = 0U; i < n; i++) { data[i] = mockResponse[i]; }
    }

    return mockTransferOk;
}

bool HW_I2C_memWrite(HW_I2C_bus_E bus, uint8_t devAddr7, uint16_t memAddr,
                     HW_I2C_memAddrSize_E memAddrSize, uint8_t * data, size_t length)
{
    mockLastCall.called      = true;
    mockLastCall.isWrite     = true;
    mockLastCall.bus         = bus;
    mockLastCall.devAddr7    = devAddr7;
    mockLastCall.memAddr     = memAddr;
    mockLastCall.memAddrSize = memAddrSize;
    mockLastCall.length      = length;

    if (data != NULL)
    {
        const size_t n = (length < MOCK_HW_I2C_MAX_BYTES) ? length : MOCK_HW_I2C_MAX_BYTES;
        for (size_t i = 0U; i < n; i++) { mockLastCall.data[i] = data[i]; }
    }

    return mockTransferOk;
}
