/* Includes */

#include "lib_types.h"

#include "HW_OPAMP.h"

/* Typedefs */

typedef struct
{
    // Analog volts at each channel's input pin — world state, not driver
    // state: the SIL pokes it (DWARF) before boot; init samples it, never
    // clears it.
    float32_t inputVolts[HW_OPAMP_CHANNEL_COUNT];

    // Volts each amplifier drives onto its internal ADC input: input x
    // configured gain, computed at init (a started amplifier is
    // set-and-forget analog — init is the driver's only execution point).
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
