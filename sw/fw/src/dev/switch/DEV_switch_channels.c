/* Includes */
#include "DEV_switch.h"  // pulls lib_types (needed before lib_utils.h)
#include "lib_utils.h"   // COUNTOF

/* Private Data Definitions */

static const DEV_switch_channelConfig_S DEV_switch_channelConfig[] =
{
    [DEV_SWITCH_CHANNEL_USER_BUTTON] =
    {
        // PB10 user button, internal pull-up -> idle HIGH, pressed reads LOW.
        // 20 consecutive 1 ms samples (20 ms) to accept a change.
        .port = HW_GPIO_PORT_B,
        .pin = 0x0400U,   // GPIO_PIN_10
        .activeLevel = HW_GPIO_LEVEL_LOW,
        .debounceCount = 20U,
    },
};

const DEV_switch_config_S DEV_switch_config =
{
    .channels = DEV_switch_channelConfig,
    .numChannels = COUNTOF(DEV_switch_channelConfig),
};
