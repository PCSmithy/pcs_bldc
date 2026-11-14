/* Includes */

#include "lib_types.h"

#include "HW_OPAMP.h"

/* Typedefs */

typedef struct
{
    const HW_OPAMP_config_S * config;

    // Analog volts at each channel's input pin — world state, not driver
    // state: the SIL pokes it (DWARF); init never clears it.
    float32_t inputVolts[HW_OPAMP_CHANNEL_COUNT];

    // Volts each amplifier drives onto its internal ADC input: input x
    // configured gain, taken at init and at each conversion of that input.
    float32_t outputVolts[HW_OPAMP_CHANNEL_COUNT];

    bool initialized;
} HW_OPAMP_data_S;

/* Private Data Definitions */

static HW_OPAMP_data_S HW_OPAMP_data;
static HW_OPAMP_data_S * const data = &HW_OPAMP_data;

/* Public Function Definitions */

// [impl->fw~hal_opamp_001~1]
// [impl->fw~hal_opamp_002~1]
bool HW_OPAMP_init(const HW_OPAMP_config_S * const config)
{
    bool ret = false;
    if ((config != NULL) &&
        (config->channels != NULL) &&
        (config->numChannels <= HW_OPAMP_CHANNEL_COUNT))
    {
        data->config = config;
        for (size_t ch = 0U; ch < HW_OPAMP_CHANNEL_COUNT; ch++)
        {
            data->outputVolts[ch] = 0.0f;
        }
        for (size_t ch = 0U; ch < config->numChannels; ch++)
        {
            data->outputVolts[ch] = data->inputVolts[ch] * config->channels[ch].gain;
        }
        data->initialized = true;
        ret = true;
    }
    return ret;
}

// [impl->fw~hal_opamp_002~1]
bool HW_OPAMP_getOutputVolts(HW_OPAMP_channels_E channel, float32_t * const out)
{
    bool ret = false;
    if ((out != NULL) &&
        (data->initialized) &&
        ((size_t)channel < data->config->numChannels))
    {
        data->outputVolts[channel] = data->inputVolts[channel] * data->config->channels[channel].gain;
        *out = data->outputVolts[channel];
        ret = true;
    }
    return ret;
}
