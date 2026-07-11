#include "HW_ADC.h"
#include "mock_HW_ADC.h"

// Regular-sequence input count on the real driver (STM32G4 ADC1/2 span
// channels 0..18). Sized to match so any board IN# used in a config is valid.
#define MOCK_ADC_INPUTS (19U)

static float32_t mockVolts[HW_ADC_CHANNEL_COUNT][MOCK_ADC_INPUTS];
static bool      mockReadable[HW_ADC_CHANNEL_COUNT][MOCK_ADC_INPUTS];

void mock_HW_ADC_reset(void)
{
    for (uint32_t ch = 0U; ch < HW_ADC_CHANNEL_COUNT; ch++)
    {
        for (uint32_t in = 0U; in < MOCK_ADC_INPUTS; in++)
        {
            mockVolts[ch][in] = 0.0f;
            mockReadable[ch][in] = false;
        }
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
