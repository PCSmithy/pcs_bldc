/* Includes */
#include "dev_gateDriver.h"  // pulls lib_types (needed before lib_utils.h)
#include "lib_utils.h"

/* Private Data Definitions */

static const dev_gateDriver_channelConfig_S dev_gateDriver_channelConfig[] =
{
    [DEV_GATEDRIVER_CHANNEL_MAIN] =
    {
        .ioDevice = IO_I2C_DEVICE_GATEDRIVER,

        // POWMNG: VCC buck to 10 V (VCC_VAL=01); regulators at defaults.
        .powmng = 0x01U,
        // LOGIC: datasheet defaults — VDS deglitch 6 us, min deadtime on,
        // interlock on (reserved bits [6:4] read/write as 1).
        .logic  = 0x73U,
        // READY: defaults — reports standby request + VCC UVLO.
        .ready  = 0x09U,
        // NFAULT: defaults — reports VDS protection, thermal shutdown, and
        // VCC UVLO (reserved bits [6:3] read/write as 1).
        .nfault = 0x7FU,
    },
};

const dev_gateDriver_config_S dev_gateDriver_config =
{
    .channels = dev_gateDriver_channelConfig,
    .numChannels = COUNTOF(dev_gateDriver_channelConfig),
};
