/* Includes */

#include "lib_types.h"

#include "HW_OPAMP.h"
#include "HW_OPAMP_sim.h"

/* Typedefs */

typedef struct
{
    const HW_OPAMP_config_S * config;

    // Analog voltage present at each channel's input pin (set by the SIL
    // hook). getOutputVolts multiplies it by the configured gain.
    float32_t inputVolts[HW_OPAMP_CHANNEL_COUNT];

    bool initialized;
} HW_OPAMP_data_S;

/* Private Data Definitions */

static HW_OPAMP_data_S HW_OPAMP_data;
static HW_OPAMP_data_S * const data = &HW_OPAMP_data;

/* Public Function Definitions */

// [impl->fw~hal_opamp_001~1]
bool HW_OPAMP_init(const HW_OPAMP_config_S * const config)
{
    bool ret = false;
    if ((config != NULL) &&
        (config->channels != NULL) &&
        (config->numChannels <= HW_OPAMP_CHANNEL_COUNT))
    {
        data->config      = config;
        data->initialized = true;
        ret = true;
    }
    return ret;
}

void HW_OPAMP_sim_reset(void)
{
    *data = (HW_OPAMP_data_S){ 0 };
}

void HW_OPAMP_sim_setInputVolts(HW_OPAMP_channels_E channel, float32_t volts)
{
    if (channel < HW_OPAMP_CHANNEL_COUNT)
    {
        data->inputVolts[channel] = volts;
    }
}

// [impl->fw~hal_opamp_002~1]
bool HW_OPAMP_sim_getOutputVolts(HW_OPAMP_channels_E channel, float32_t * const out)
{
    bool ret = false;
    if ((out != NULL) &&
        (data->initialized) &&
        (channel < data->config->numChannels))
    {
        *out = data->inputVolts[channel] * data->config->channels[channel].gain;
        ret = true;
    }
    return ret;
}
