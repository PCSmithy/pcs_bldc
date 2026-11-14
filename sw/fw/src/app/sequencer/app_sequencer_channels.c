/* Includes */
#include "lib_utils.h"

#include "app_sequencer.h"

/* Private Data Definitions */

static app_sequencer_setpoint_S app_sequencer_storage[2U * APP_SEQUENCER_CAPACITY];

static const app_sequencer_channelConfig_S app_sequencer_channelConfig[] =
{
    [APP_SEQUENCER_CHANNEL_MAIN] =
    {
        .motor = APP_MOTORCONTROL_CHANNEL_MAIN,
        .storage = app_sequencer_storage,
        .capacity = APP_SEQUENCER_CAPACITY,
    },
};

/* Public Data Definitions */

const app_sequencer_config_S app_sequencer_config =
{
    .channels = app_sequencer_channelConfig,
    .numChannels = COUNTOF(app_sequencer_channelConfig),
};
