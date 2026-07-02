/* Includes */
#include "dev_switch.h"  // pulls lib_types (needed before lib_utils.h)
#include "lib_utils.h"   // COUNTOF

/* Private Data Definitions */

static const dev_switch_channelConfig_S dev_switch_channelConfig[] =
{
    [DEV_SWITCH_CHANNEL_USER_BUTTON] =
    {
        .type = DEV_SWITCH_TYPE_HW_DIGIN,
        .hwDigIn =
        {
            // PB10 user button, internal pull-up -> idle HIGH, pressed reads LOW.
            .port = HW_GPIO_PORT_B,
            .pin = 0x0400U,   // GPIO_PIN_10
            .activeLevel = HW_GPIO_LEVEL_LOW,
        },
        .debounce_ms = 20U,   // input must hold steady 20 ms to latch
    },
};

const dev_switch_config_S dev_switch_config =
{
    .channels = dev_switch_channelConfig,
    .numChannels = COUNTOF(dev_switch_channelConfig),
};
