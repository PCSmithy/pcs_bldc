#include "HW_TIM.h"
#include "mock_HW_TIM.h"

static uint32_t            mockPeriod[HW_TIM_CHANNEL_COUNT];
static uint32_t            mockCompare[HW_TIM_CHANNEL_COUNT];
static bool                mockOutputEnabled[HW_TIM_CHANNEL_COUNT];
static HW_TIM_peripheral_E mockPeripheral[HW_TIM_CHANNEL_COUNT];
static bool                mockMoe[HW_TIM_PERIPHERAL_COUNT];
static uint32_t            mockBreakFlagsClearCount[HW_TIM_PERIPHERAL_COUNT];

void mock_HW_TIM_reset(uint32_t period)
{
    for (uint32_t ch = 0U; ch < HW_TIM_CHANNEL_COUNT; ch++)
    {
        mockPeriod[ch] = period;
        mockCompare[ch] = 0U;
        mockOutputEnabled[ch] = false;
        // Bridge phases share peripheral 1; OTHER lives on peripheral 2.
        mockPeripheral[ch] = (ch == (uint32_t)HW_TIM_CHANNEL_OTHER)
                                 ? HW_TIM_PERIPHERAL_2
                                 : HW_TIM_PERIPHERAL_1;
    }
    for (uint32_t p = 0U; p < HW_TIM_PERIPHERAL_COUNT; p++)
    {
        mockMoe[p] = false;
        mockBreakFlagsClearCount[p] = 0U;
    }
}

void mock_HW_TIM_setPeriod(HW_TIM_channels_E channel, uint32_t period)
{
    if (channel < HW_TIM_CHANNEL_COUNT)
    {
        mockPeriod[channel] = period;
    }
}

void mock_HW_TIM_setPeripheral(HW_TIM_channels_E channel, HW_TIM_peripheral_E peripheral)
{
    if (channel < HW_TIM_CHANNEL_COUNT)
    {
        mockPeripheral[channel] = peripheral;
    }
}

void mock_HW_TIM_assertBreak(HW_TIM_peripheral_E peripheral)
{
    if (peripheral < HW_TIM_PERIPHERAL_COUNT)
    {
        mockMoe[peripheral] = false;
    }
}

uint32_t mock_HW_TIM_getCompare(HW_TIM_channels_E channel)
{
    uint32_t compare = 0U;
    if (channel < HW_TIM_CHANNEL_COUNT)
    {
        compare = mockCompare[channel];
    }
    return compare;
}

bool mock_HW_TIM_getOutputEnabled(HW_TIM_channels_E channel)
{
    bool enabled = false;
    if (channel < HW_TIM_CHANNEL_COUNT)
    {
        enabled = mockOutputEnabled[channel];
    }
    return enabled;
}

bool mock_HW_TIM_getMoe(HW_TIM_peripheral_E peripheral)
{
    bool moe = false;
    if (peripheral < HW_TIM_PERIPHERAL_COUNT)
    {
        moe = mockMoe[peripheral];
    }
    return moe;
}

uint32_t mock_HW_TIM_getBreakFlagsClearCount(HW_TIM_peripheral_E peripheral)
{
    uint32_t count = 0U;
    if (peripheral < HW_TIM_PERIPHERAL_COUNT)
    {
        count = mockBreakFlagsClearCount[peripheral];
    }
    return count;
}

/* ---- mocked HW_TIM surface ---- */

bool HW_TIM_getPeripheral(HW_TIM_channels_E channel, HW_TIM_peripheral_E * const out)
{
    bool ret = false;
    if ((out != NULL) && (channel < HW_TIM_CHANNEL_COUNT))
    {
        *out = mockPeripheral[channel];
        ret = true;
    }
    return ret;
}

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

bool HW_TIM_setCompare(HW_TIM_channels_E channel, uint32_t counts)
{
    bool ret = false;
    if ((channel < HW_TIM_CHANNEL_COUNT) && (counts <= mockPeriod[channel]))
    {
        mockCompare[channel] = counts;
        ret = true;
    }
    return ret;
}

bool HW_TIM_setOutputEnabled(HW_TIM_channels_E channel, bool enabled)
{
    bool ret = false;
    if (channel < HW_TIM_CHANNEL_COUNT)
    {
        mockOutputEnabled[channel] = enabled;
        ret = true;
    }
    return ret;
}

bool HW_TIM_setMainOutputEnabled(HW_TIM_peripheral_E peripheral, bool enabled)
{
    bool ret = false;
    if (peripheral < HW_TIM_PERIPHERAL_COUNT)
    {
        mockMoe[peripheral] = enabled;
        ret = true;
    }
    return ret;
}

bool HW_TIM_getMainOutputEnabled(HW_TIM_peripheral_E peripheral, bool * const enabled)
{
    bool ret = false;
    if ((enabled != NULL) && (peripheral < HW_TIM_PERIPHERAL_COUNT))
    {
        *enabled = mockMoe[peripheral];
        ret = true;
    }
    return ret;
}

bool HW_TIM_clearBreakFlags(HW_TIM_peripheral_E peripheral)
{
    bool ret = false;
    if (peripheral < HW_TIM_PERIPHERAL_COUNT)
    {
        mockBreakFlagsClearCount[peripheral]++;
        ret = true;
    }
    return ret;
}
