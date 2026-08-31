/* Includes */
#include "IO_COBSFrame.h"
#include "lib_utils.h"   // COUNTOF

/* Private Data Definitions */

static const IO_COBSFrame_channelConfig_S IO_COBSFrame_channelConfig[] =
{
    [IO_COBSFRAME_CHANNEL_CDC] =
    {
        .serialChannel = IO_SERIAL_CHANNEL_CDC,
        .maxFrameLen   = IO_COBSFRAME_MAX_PAYLOAD,
    },
};

const IO_COBSFrame_config_S IO_COBSFrame_config =
{
    .channels    = IO_COBSFrame_channelConfig,
    .numChannels = COUNTOF(IO_COBSFrame_channelConfig),
};
