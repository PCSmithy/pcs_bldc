/* Includes */
#include "dev_switch.h"
#include "HW_GPIO.h"
#include "lib_timer.h"

/* Typedefs */

typedef struct
{
    dev_switch_state_E state;
    lib_timer_channel_S debounce;
} dev_switch_channelData_S;

typedef struct
{
    const dev_switch_config_S * config;
    dev_switch_channelData_S channels[DEV_SWITCH_CHANNEL_COUNT];
} dev_switch_data_S;

/* Private Data Definitions */

static dev_switch_data_S dev_switch_data;
static dev_switch_data_S * const data = &dev_switch_data;

/* Public Function Definitions */

bool dev_switch_init(const dev_switch_config_S * const config)
{
    bool success = false;
    if (config != NULL)
    {
        bool channelsValid = true;
        for (dev_switch_channel_E channel = (dev_switch_channel_E)0U; channel < DEV_SWITCH_CHANNEL_COUNT; channel++)
        {
            switch (config->channels[channel].type)
            {
            case DEV_SWITCH_TYPE_HW_DIGIN:
                channelsValid &= (config->channels[channel].hwDigIn.port < HW_GPIO_PORT_COUNT);
                break;
            case DEV_SWITCH_TYPE_NETWORK:
                channelsValid &= (config->channels[channel].network.getState != NULL);
                break;

            case DEV_SWITCH_TYPE_COUNT:
            default:
                channelsValid = false;
                break;
            }
        }

        if (channelsValid)
        {
            data->config = config;
            // Establish a known runtime state: every channel starts inactive
            // with a cleared debounce counter, so a re-init can't inherit a
            // stale debounced state.
            for (dev_switch_channel_E channel = (dev_switch_channel_E)0U; channel < DEV_SWITCH_CHANNEL_COUNT; channel++)
            {
                data->channels[channel].state   = DEV_SWITCH_STATE_UNKNOWN;

                lib_timer_init(&data->channels[channel].debounce, LIB_TIMER_PRECISION_MS, config->channels[channel].debounce_ms);
            }
            success = true;
        }
    }
    return success;
}

void dev_switch_run1ms(void)
{
    if (data->config != NULL)
    {
        for (dev_switch_channel_E channel = (dev_switch_channel_E)0U; channel < DEV_SWITCH_CHANNEL_COUNT; channel++)
        {
            const dev_switch_channelConfig_S * const channelConfig = &data->config->channels[channel];
            dev_switch_channelData_S * const channelData = &data->channels[channel];

            dev_switch_state_E stateRaw = DEV_SWITCH_STATE_UNKNOWN;

            switch (channelConfig->type)
            {
                case DEV_SWITCH_TYPE_HW_DIGIN:
                {
                    const dev_switch_hwDigInConfig_S * const hw = &channelConfig->hwDigIn;
                    const HW_GPIO_level_E level = HW_GPIO_readCached(hw->port, hw->pin);
                    stateRaw = (level == hw->activeLevel) ? DEV_SWITCH_STATE_ACTIVE : DEV_SWITCH_STATE_INACTIVE;
                    break;
                }

                case DEV_SWITCH_TYPE_NETWORK:
                    stateRaw = channelConfig->network.getState();
                    break;

                case DEV_SWITCH_TYPE_COUNT:
                default:
                    break;
            }

            // Q: Should we only debounce the rising edge?
            const bool isDebounced = lib_timer_runTimerWithEnable(&channelData->debounce, (bool)(stateRaw != channelData->state)) == LIB_TIMER_STATE_EXPIRED;
            if (isDebounced)
            {
                channelData->state = stateRaw;
            }
        }
    }
}

dev_switch_state_E dev_switch_getState(dev_switch_channel_E channel)
{
    dev_switch_state_E state = DEV_SWITCH_STATE_UNKNOWN;

    if ((data->config != NULL) && (channel < DEV_SWITCH_CHANNEL_COUNT))
    {
        state = data->channels[channel].state;
    }
    return state;
}


bool dev_switch_isActive(dev_switch_channel_E channel)
{
    bool active = false;
    if ((data->config != NULL) && (channel < DEV_SWITCH_CHANNEL_COUNT))
    {
        active = data->channels[channel].state == DEV_SWITCH_STATE_ACTIVE;
    }
    return active;
}
