#include "dev_gateDriver.h"
#include "mock_dev_gateDriver.h"

static bool mockOperational[DEV_GATEDRIVER_CHANNEL_COUNT];

void mock_dev_gateDriver_reset(void)
{
    for (uint32_t ch = 0U; ch < DEV_GATEDRIVER_CHANNEL_COUNT; ch++)
    {
        mockOperational[ch] = false;
    }
}

void mock_dev_gateDriver_setOperational(dev_gateDriver_channel_E channel, bool operational)
{
    if (channel < DEV_GATEDRIVER_CHANNEL_COUNT)
    {
        mockOperational[channel] = operational;
    }
}

/* ---- mocked dev_gateDriver surface ---- */

bool dev_gateDriver_isOperational(dev_gateDriver_channel_E channel)
{
    bool ret = false;
    if (channel < DEV_GATEDRIVER_CHANNEL_COUNT)
    {
        ret = mockOperational[channel];
    }
    return ret;
}
