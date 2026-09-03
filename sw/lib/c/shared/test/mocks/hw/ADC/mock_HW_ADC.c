#include "HW_ADC.h"
#include "mock_HW_ADC.h"

// Regular-sequence input count on the real driver (STM32G4 ADC1/2 span
// channels 0..18). Sized to match so any board IN# used in a config is valid.
#define MOCK_ADC_INPUTS (19U)

static float32_t mockVolts[HW_ADC_CHANNEL_COUNT][MOCK_ADC_INPUTS];
static bool      mockReadable[HW_ADC_CHANNEL_COUNT][MOCK_ADC_INPUTS];

static float32_t mockInjectedVolts[HW_ADC_CHANNEL_COUNT][HW_ADC_INJECTED_INPUTS_PER_CHANNEL];
static bool      mockInjectedReadable[HW_ADC_CHANNEL_COUNT][HW_ADC_INJECTED_INPUTS_PER_CHANNEL];

static HW_ADC_injectedCallback_F mockCallback[HW_ADC_CHANNEL_COUNT];
static void *                    mockCallbackContext[HW_ADC_CHANNEL_COUNT];
static uint32_t                  mockRegistrations[HW_ADC_CHANNEL_COUNT];
static bool                      mockRegistrationFails[HW_ADC_CHANNEL_COUNT];

void mock_HW_ADC_reset(void)
{
    for (uint32_t ch = 0U; ch < HW_ADC_CHANNEL_COUNT; ch++)
    {
        for (uint32_t in = 0U; in < MOCK_ADC_INPUTS; in++)
        {
            mockVolts[ch][in] = 0.0f;
            mockReadable[ch][in] = false;
        }
        for (uint32_t inj = 0U; inj < HW_ADC_INJECTED_INPUTS_PER_CHANNEL; inj++)
        {
            mockInjectedVolts[ch][inj] = 0.0f;
            mockInjectedReadable[ch][inj] = false;
        }
        mockCallback[ch] = NULL;
        mockCallbackContext[ch] = NULL;
        mockRegistrations[ch] = 0U;
        mockRegistrationFails[ch] = false;
    }
}

void mock_HW_ADC_setVolts(HW_ADC_channels_E channel, uint8_t inputIndex, float32_t volts)
{
    if ((channel < HW_ADC_CHANNEL_COUNT) && (inputIndex < MOCK_ADC_INPUTS))
    {
        mockVolts[channel][inputIndex] = volts;
        mockReadable[channel][inputIndex] = true;
    }
}

void mock_HW_ADC_setInjectedVolts(HW_ADC_channels_E channel, uint8_t injectedIndex, float32_t volts)
{
    if ((channel < HW_ADC_CHANNEL_COUNT) && (injectedIndex < HW_ADC_INJECTED_INPUTS_PER_CHANNEL))
    {
        mockInjectedVolts[channel][injectedIndex] = volts;
        mockInjectedReadable[channel][injectedIndex] = true;
    }
}

void mock_HW_ADC_failRegistration(HW_ADC_channels_E channel)
{
    if (channel < HW_ADC_CHANNEL_COUNT)
    {
        mockRegistrationFails[channel] = true;
    }
}

uint32_t mock_HW_ADC_getRegistrationCount(HW_ADC_channels_E channel)
{
    uint32_t count = 0U;
    if (channel < HW_ADC_CHANNEL_COUNT)
    {
        count = mockRegistrations[channel];
    }
    return count;
}

void mock_HW_ADC_fireInjected(HW_ADC_channels_E channel, HW_ADC_conversionStatus_E status)
{
    if ((channel < HW_ADC_CHANNEL_COUNT) && (mockCallback[channel] != NULL))
    {
        mockCallback[channel](channel, status, mockCallbackContext[channel]);
    }
}

/* ---- mocked HW_ADC surface ---- */

bool HW_ADC_getVolts(HW_ADC_channels_E channel, uint8_t inputIndex, float32_t * const out)
{
    bool ret = false;
    if ((out != NULL) &&
        (channel < HW_ADC_CHANNEL_COUNT) &&
        (inputIndex < MOCK_ADC_INPUTS) &&
        (mockReadable[channel][inputIndex]))
    {
        *out = mockVolts[channel][inputIndex];
        ret = true;
    }
    return ret;
}

bool HW_ADC_getInjectedVolts(HW_ADC_channels_E channel, uint8_t injectedIndex, float32_t * const out)
{
    bool ret = false;
    if ((out != NULL) &&
        (channel < HW_ADC_CHANNEL_COUNT) &&
        (injectedIndex < HW_ADC_INJECTED_INPUTS_PER_CHANNEL) &&
        (mockInjectedReadable[channel][injectedIndex]))
    {
        *out = mockInjectedVolts[channel][injectedIndex];
        ret = true;
    }
    return ret;
}

bool HW_ADC_registerInjectedCallback(HW_ADC_channels_E channel,
                                     HW_ADC_injectedCallback_F callback,
                                     void * context)
{
    bool ret = false;
    if ((channel < HW_ADC_CHANNEL_COUNT) &&
        (callback != NULL) &&
        (!mockRegistrationFails[channel]))
    {
        mockCallback[channel] = callback;
        mockCallbackContext[channel] = context;
        mockRegistrations[channel]++;
        ret = true;
    }
    return ret;
}
