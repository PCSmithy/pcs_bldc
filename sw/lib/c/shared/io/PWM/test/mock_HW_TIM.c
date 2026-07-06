#include "HW_TIM.h"
#include "mock_HW_TIM.h"

static uint32_t mockPeriod[HW_TIM_CHANNEL_COUNT];
static uint32_t mockCompare[HW_TIM_CHANNEL_COUNT][HW_TIM_OC_UNITS_PER_CHANNEL];
static bool     mockOutputEnabled[HW_TIM_CHANNEL_COUNT][HW_TIM_OC_UNITS_PER_CHANNEL];
static bool     mockMoe[HW_TIM_CHANNEL_COUNT];

void mock_HW_TIM_reset(uint32_t period)
{
    for (uint32_t ch = 0U; ch < HW_TIM_CHANNEL_COUNT; ch++)
    {
        mockPeriod[ch] = period;
        mockMoe[ch] = false;
        for (uint32_t unit = 0U; unit < HW_TIM_OC_UNITS_PER_CHANNEL; unit++)
        {
            mockCompare[ch][unit] = 0U;
            mockOutputEnabled[ch][unit] = false;
        }
    }
}

void mock_HW_TIM_setPeriod(HW_TIM_channels_E channel, uint32_t period)
{
    if (channel < HW_TIM_CHANNEL_COUNT)
    {
        mockPeriod[channel] = period;
    }
}

void mock_HW_TIM_assertBreak(HW_TIM_channels_E channel)
{
    if (channel < HW_TIM_CHANNEL_COUNT)
    {
        mockMoe[channel] = false;
    }
}

uint32_t mock_HW_TIM_getCompare(HW_TIM_channels_E channel, uint8_t ocUnit)
{
    uint32_t compare = 0U;
    if ((channel < HW_TIM_CHANNEL_COUNT) && (ocUnit < HW_TIM_OC_UNITS_PER_CHANNEL))
    {
        compare = mockCompare[channel][ocUnit];
    }
    return compare;
}

bool mock_HW_TIM_getOutputEnabled(HW_TIM_channels_E channel, uint8_t ocUnit)
{
    bool enabled = false;
    if ((channel < HW_TIM_CHANNEL_COUNT) && (ocUnit < HW_TIM_OC_UNITS_PER_CHANNEL))
    {
        enabled = mockOutputEnabled[channel][ocUnit];
    }
    return enabled;
}

bool mock_HW_TIM_getMoe(HW_TIM_channels_E channel)
{
    bool moe = false;
    if (channel < HW_TIM_CHANNEL_COUNT)
    {
        moe = mockMoe[channel];
    }
    return moe;
}

/* ---- mocked HW_TIM surface ---- */

bool HW_TIM_getPeriod(HW_TIM_channels_E channel, uint32_t * const out)
{
    bool ret = false;
    if ((out != NULL) && (channel < HW_TIM_CHANNEL_COUNT))
    {
        *out = mockPeriod[channel];
        ret = true;
    }
    return ret;
}

bool HW_TIM_setCompare(HW_TIM_channels_E channel, uint8_t ocUnit, uint32_t counts)
{
    bool ret = false;
    if ((channel < HW_TIM_CHANNEL_COUNT) &&
        (ocUnit < HW_TIM_OC_UNITS_PER_CHANNEL) &&
        (counts <= mockPeriod[channel]))
    {
        mockCompare[channel][ocUnit] = counts;
        ret = true;
    }
    return ret;
}

bool HW_TIM_setOutputEnabled(HW_TIM_channels_E channel, uint8_t ocUnit, bool enabled)
{
    bool ret = false;
    if ((channel < HW_TIM_CHANNEL_COUNT) && (ocUnit < HW_TIM_OC_UNITS_PER_CHANNEL))
    {
        mockOutputEnabled[channel][ocUnit] = enabled;
        ret = true;
    }
    return ret;
}

bool HW_TIM_setMainOutputEnabled(HW_TIM_channels_E channel, bool enabled)
{
    bool ret = false;
    if (channel < HW_TIM_CHANNEL_COUNT)
    {
        mockMoe[channel] = enabled;
        ret = true;
    }
    return ret;
}

bool HW_TIM_getMainOutputEnabled(HW_TIM_channels_E channel, bool * const enabled)
{
    bool ret = false;
    if ((enabled != NULL) && (channel < HW_TIM_CHANNEL_COUNT))
    {
        *enabled = mockMoe[channel];
        ret = true;
    }
    return ret;
}
