/* Includes */
#include "DEV_switch.h"
#include "HW_GPIO.h"

/* Typedefs */

typedef struct
{
    bool     active;   // current debounced state
    uint16_t counter;  // consecutive samples disagreeing with `active`
} DEV_switch_channelData_S;

typedef struct
{
    const DEV_switch_config_S * config;
    DEV_switch_channelData_S channels[DEV_SWITCH_CHANNEL_COUNT];
} DEV_switch_data_S;

/* Private Data Definitions */

static DEV_switch_data_S DEV_switch_data;
static DEV_switch_data_S * const data = &DEV_switch_data;

/* Public Function Definitions */

bool DEV_switch_init(const DEV_switch_config_S * const config)
{
    bool success = false;
    if (config != NULL)
    {
        bool channelsValid = true;
        for (DEV_switch_channel_E channel = (DEV_switch_channel_E)0U; channel < DEV_SWITCH_CHANNEL_COUNT; channel++)
        {
            channelsValid &= (config->channels[channel].port < HW_GPIO_PORT_COUNT);
        }

        if (channelsValid)
        {
            data->config = config;
            // Establish a known runtime state: every channel starts inactive
            // with a cleared debounce counter, so a re-init can't inherit a
            // stale debounced state.
            for (DEV_switch_channel_E channel = (DEV_switch_channel_E)0U; channel < DEV_SWITCH_CHANNEL_COUNT; channel++)
            {
                data->channels[channel].active  = false;
                data->channels[channel].counter = 0U;
            }
            success = true;
        }
    }
    return success;
}

void DEV_switch_run1ms(void)
{
    if (data->config != NULL)
    {
        for (DEV_switch_channel_E channel = (DEV_switch_channel_E)0U; channel < DEV_SWITCH_CHANNEL_COUNT; channel++)
        {
            const DEV_switch_channelConfig_S * const channelConfig = &data->config->channels[channel];
            DEV_switch_channelData_S * const channelData = &data->channels[channel];

            const HW_GPIO_level_E level = HW_GPIO_readCached(channelConfig->port, channelConfig->pin);
            const bool rawActive = (level == channelConfig->activeLevel);

            if (rawActive == channelData->active)
            {
                // Reading agrees with the held state; nothing pending.
                channelData->counter = 0U;
            }
            else
            {
                // Candidate change: flip only after debounceCount consecutive
                // samples of the new level, so contact bounce is rejected.
                channelData->counter++;
                if (channelData->counter >= channelConfig->debounceCount)
                {
                    channelData->active  = rawActive;
                    channelData->counter = 0U;
                }
            }
        }
    }
}

bool DEV_switch_isActive(DEV_switch_channel_E channel)
{
    bool active = false;
    if ((data->config != NULL) && (channel < DEV_SWITCH_CHANNEL_COUNT))
    {
        active = data->channels[channel].active;
    }
    return active;
}
