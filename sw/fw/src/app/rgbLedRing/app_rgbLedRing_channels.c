/* Includes */
#include "app_rgbLedRing.h"  // pulls lib_types + the IO/dev channel enums
#include "lib_utils.h"       // COUNTOF

/* Private Data Definitions */

// Board wiring for the single status ring: the SK6805 ring string, steered by
// the dial (knob) and motor encoders. Mode cycling comes from app_userControls
// via app_rgbLedRing_cycleMode, so no button is wired here.
static const app_rgbLedRing_channelConfig_S app_rgbLedRing_channelConfig[] =
{
    [APP_RGBLEDRING_CHANNEL_RING] =
    {
        .ledChannel                = IO_SK6805_CHANNEL_RING,
        .dialChannel               = IO_AS5048_CHANNEL_DIAL,
        .motorChannel              = IO_AS5048_CHANNEL_MOTOR,
        .motorControlChannel       = APP_MOTORCONTROL_CHANNEL_MAIN,
        .speedoFullScale_radPerSec = APP_MOTORCONTROL_MAX_VELOCITY_RAD_PER_SEC,
        .pixelCount                = IO_SK6805_PIXEL_COUNT,
    },
};

const app_rgbLedRing_config_S app_rgbLedRing_config =
{
    .channels = app_rgbLedRing_channelConfig,
    .numChannels = COUNTOF(app_rgbLedRing_channelConfig),
};
