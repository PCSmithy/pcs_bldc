/* Includes */
#include "dev_gateDriver.h"

/* Typedefs */

typedef struct
{
    const dev_gateDriver_config_S * config;
    dev_gateDriver_snapshot_S channels[DEV_GATEDRIVER_CHANNEL_COUNT];
} dev_gateDriver_data_S;

/* Private Data Definitions */

static dev_gateDriver_data_S dev_gateDriver_data;
static dev_gateDriver_data_S * const data = &dev_gateDriver_data;

/* Private Function Definitions */

static void dev_gateDriver_private_clear(dev_gateDriver_snapshot_S * const channelData)
{
    channelData->configured      = false;
    channelData->statusOk        = false;
    channelData->statusRaw       = 0U;
    channelData->locked          = false;
    channelData->resetLatched    = false;
    channelData->vdsProtection   = false;
    channelData->thermalShutdown = false;
    channelData->vccUndervoltage = false;
}

// [impl->fw~mc_005~1]
static bool dev_gateDriver_private_clearFaults(IO_i2c_device_E ioDevice)
{
    uint8_t clearAll = DEV_GATEDRIVER_CLEAR_ALL;
    return IO_i2c_writeReg(ioDevice, (uint16_t)DEV_GATEDRIVER_REG_CLEAR, &clearAll, sizeof(clearAll));
}

// Write one register and verify it by readback.
// [impl->fw~mc_002~1]
static bool dev_gateDriver_private_writeVerify(IO_i2c_device_E ioDevice,
                                               dev_gateDriver_register_E reg, uint8_t value)
{
    uint8_t writeValue = value;
    uint8_t readback   = (uint8_t)~value;

    bool ok = true;
    ok &= IO_i2c_writeReg(ioDevice, (uint16_t)reg, &writeValue, sizeof(writeValue));
    ok &= IO_i2c_readReg(ioDevice, (uint16_t)reg, &readback, sizeof(readback));
    ok &= (readback == value);
    return ok;
}

// The configuration sequence: unlock, program + verify each configuration
// register, relock, clear the power-up-latched faults. The relock and clear
// run even after an earlier failure so the protected registers are never left
// unlocked.
// [impl->fw~mc_002~1]
static void dev_gateDriver_private_configure(dev_gateDriver_channel_E channel)
{
    const dev_gateDriver_channelConfig_S * const channelConfig = &data->config->channels[channel];
    const IO_i2c_device_E ioDevice = channelConfig->ioDevice;

    uint8_t unlockValue = DEV_GATEDRIVER_LOCK_UNLOCK;
    uint8_t relockValue = DEV_GATEDRIVER_LOCK_RELOCK;

    bool ok = true;
    ok &= IO_i2c_writeReg(ioDevice, (uint16_t)DEV_GATEDRIVER_REG_LOCK, &unlockValue, sizeof(unlockValue));
    ok &= dev_gateDriver_private_writeVerify(ioDevice, DEV_GATEDRIVER_REG_POWMNG, channelConfig->powmng);
    ok &= dev_gateDriver_private_writeVerify(ioDevice, DEV_GATEDRIVER_REG_LOGIC, channelConfig->logic);
    ok &= dev_gateDriver_private_writeVerify(ioDevice, DEV_GATEDRIVER_REG_READY, channelConfig->ready);
    ok &= dev_gateDriver_private_writeVerify(ioDevice, DEV_GATEDRIVER_REG_NFAULT, channelConfig->nfault);
    ok &= IO_i2c_writeReg(ioDevice, (uint16_t)DEV_GATEDRIVER_REG_LOCK, &relockValue, sizeof(relockValue));
    ok &= dev_gateDriver_private_clearFaults(ioDevice);

    data->channels[channel].configured = ok;
}

// [impl->fw~mc_003~1]
static void dev_gateDriver_private_readStatus(dev_gateDriver_channel_E channel)
{
    const IO_i2c_device_E ioDevice = data->config->channels[channel].ioDevice;
    dev_gateDriver_snapshot_S * const channelData = &data->channels[channel];

    uint8_t status = 0U;
    if (IO_i2c_readReg(ioDevice, (uint16_t)DEV_GATEDRIVER_REG_STATUS, &status, sizeof(status)))
    {
        channelData->statusOk        = true;
        channelData->statusRaw       = status;
        channelData->locked          = ((status >> DEV_GATEDRIVER_STATUS_LOCK_SHIFT) & 0x1U) != 0U;
        channelData->resetLatched    = ((status >> DEV_GATEDRIVER_STATUS_RESET_SHIFT) & 0x1U) != 0U;
        channelData->vdsProtection   = ((status >> DEV_GATEDRIVER_STATUS_VDS_P_SHIFT) & 0x1U) != 0U;
        channelData->thermalShutdown = ((status >> DEV_GATEDRIVER_STATUS_THSD_SHIFT) & 0x1U) != 0U;
        channelData->vccUndervoltage = ((status >> DEV_GATEDRIVER_STATUS_VCC_UVLO_SHIFT) & 0x1U) != 0U;
    }
    else
    {
        // A failed read is not a status: record the failure, keep the
        // last-good flags.
        channelData->statusOk = false;
    }
}

/* Public Function Definitions */

// [impl->fw~mc_001~1]
bool dev_gateDriver_init(const dev_gateDriver_config_S * const config)
{
    bool success = false;
    if ((config != NULL) && (config->channels != NULL) && (config->numChannels == DEV_GATEDRIVER_CHANNEL_COUNT))
    {
        bool channelsValid = true;
        for (dev_gateDriver_channel_E channel = (dev_gateDriver_channel_E)0U; channel < DEV_GATEDRIVER_CHANNEL_COUNT; channel++)
        {
            channelsValid &= (config->channels[channel].ioDevice < IO_I2C_DEVICE_COUNT);
        }

        if (channelsValid)
        {
            data->config = config;
            // Establish a known runtime state so a re-init can't inherit stale
            // status from a previous config.
            for (dev_gateDriver_channel_E channel = (dev_gateDriver_channel_E)0U; channel < DEV_GATEDRIVER_CHANNEL_COUNT; channel++)
            {
                dev_gateDriver_private_clear(&data->channels[channel]);
            }
            success = true;
        }
    }
    return success;
}

void dev_gateDriver_run200ms(void)
{
    if (data->config != NULL)
    {
        for (dev_gateDriver_channel_E channel = (dev_gateDriver_channel_E)0U; channel < DEV_GATEDRIVER_CHANNEL_COUNT; channel++)
        {
            if (!data->channels[channel].configured)
            {
                dev_gateDriver_private_configure(channel);
            }
            if (data->channels[channel].configured)
            {
                dev_gateDriver_private_readStatus(channel);
            }
        }
    }
}

// [impl->fw~mc_004~1]
bool dev_gateDriver_isConfigured(dev_gateDriver_channel_E channel)
{
    bool configured = false;
    if ((data->config != NULL) && (channel < DEV_GATEDRIVER_CHANNEL_COUNT))
    {
        configured = data->channels[channel].configured;
    }
    return configured;
}

// [impl->fw~mc_004~1]
bool dev_gateDriver_getSnapshot(dev_gateDriver_channel_E channel,
                                dev_gateDriver_snapshot_S * const snapshot)
{
    bool ret = false;
    if (snapshot != NULL)
    {
        if ((data->config != NULL) && (channel < DEV_GATEDRIVER_CHANNEL_COUNT))
        {
            *snapshot = data->channels[channel];
            ret = true;
        }
        else
        {
            dev_gateDriver_private_clear(snapshot);
        }
    }
    return ret;
}

// [impl->fw~mc_005~1]
bool dev_gateDriver_clearFaults(dev_gateDriver_channel_E channel)
{
    bool ret = false;
    if ((data->config != NULL) && (channel < DEV_GATEDRIVER_CHANNEL_COUNT))
    {
        ret = dev_gateDriver_private_clearFaults(data->config->channels[channel].ioDevice);
    }
    return ret;
}
