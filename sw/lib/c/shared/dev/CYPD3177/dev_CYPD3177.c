/* Includes */
#include "dev_CYPD3177.h"
#include "CYPD3177.h"

/* Typedefs */

typedef struct
{
    const dev_CYPD3177_config_S * config;
    dev_CYPD3177_snapshot_S channels[DEV_CYPD3177_CHANNEL_COUNT];
} dev_CYPD3177_data_S;

/* Private Data Definitions */

static dev_CYPD3177_data_S dev_CYPD3177_data;
static dev_CYPD3177_data_S * const data = &dev_CYPD3177_data;

/* Private Function Definitions */

static void dev_CYPD3177_private_clear(dev_CYPD3177_snapshot_S * const channelData)
{
    channelData->present              = false;
    channelData->deviceMode           = 0U;
    channelData->siliconId            = 0U;
    channelData->pdStatus             = 0U;
    channelData->typeCStatus          = 0U;
    channelData->busVoltageRaw        = 0U;
    channelData->currentPdo           = 0U;
    channelData->currentRdo           = 0U;
    channelData->contractActive       = false;
    channelData->negotiatedVoltage_mV = 0U;
    channelData->negotiatedCurrent_mA = 0U;
    channelData->busVoltage_mV        = 0U;
}

static uint32_t dev_CYPD3177_private_leWord(const uint8_t * const bytes, size_t length)
{
    uint32_t word = 0U;
    for (size_t i = 0U; i < length; i++)
    {
        word |= (((uint32_t)bytes[i]) << (8U * i));
    }
    return word;
}

// [impl->fw~pd_006~1]
// [impl->fw~pd_008~1]
static void dev_CYPD3177_private_fetch(dev_CYPD3177_channel_E channel)
{
    const IO_i2c_device_E ioDevice = data->config->channels[channel].ioDevice;
    dev_CYPD3177_snapshot_S * const channelData = &data->channels[channel];

    uint8_t deviceMode = 0U;
    uint8_t busVoltage = 0U;
    uint8_t siliconId[2] = { 0U, 0U };
    uint8_t pdStatus[4] = { 0U, 0U, 0U, 0U };
    uint8_t typeCStatus[4] = { 0U, 0U, 0U, 0U };
    uint8_t currentPdo[4] = { 0U, 0U, 0U, 0U };
    uint8_t currentRdo[4] = { 0U, 0U, 0U, 0U };

    bool ok = true;
    ok &= IO_i2c_readReg(ioDevice, (uint16_t)CYPD3177_REG_DEVICE_MODE, &deviceMode, sizeof(deviceMode));
    ok &= IO_i2c_readReg(ioDevice, (uint16_t)CYPD3177_REG_SILICON_ID, siliconId, sizeof(siliconId));
    ok &= IO_i2c_readReg(ioDevice, (uint16_t)CYPD3177_REG_PD_STATUS, pdStatus, sizeof(pdStatus));
    ok &= IO_i2c_readReg(ioDevice, (uint16_t)CYPD3177_REG_TYPE_C_STATUS, typeCStatus, sizeof(typeCStatus));
    ok &= IO_i2c_readReg(ioDevice, (uint16_t)CYPD3177_REG_BUS_VOLTAGE, &busVoltage, sizeof(busVoltage));
    ok &= IO_i2c_readReg(ioDevice, (uint16_t)CYPD3177_REG_CURRENT_PDO, currentPdo, sizeof(currentPdo));
    ok &= IO_i2c_readReg(ioDevice, (uint16_t)CYPD3177_REG_CURRENT_RDO, currentRdo, sizeof(currentRdo));

    if (ok)
    {
        const uint32_t pdStatusWord   = dev_CYPD3177_private_leWord(pdStatus, sizeof(pdStatus));
        const uint32_t currentPdoWord = dev_CYPD3177_private_leWord(currentPdo, sizeof(currentPdo));
        const uint32_t currentRdoWord = dev_CYPD3177_private_leWord(currentRdo, sizeof(currentRdo));

        channelData->present       = (deviceMode != 0U);
        channelData->deviceMode    = deviceMode;
        channelData->siliconId     = (uint16_t)dev_CYPD3177_private_leWord(siliconId, sizeof(siliconId));
        channelData->pdStatus      = pdStatusWord;
        channelData->typeCStatus   = dev_CYPD3177_private_leWord(typeCStatus, sizeof(typeCStatus));
        channelData->busVoltageRaw = busVoltage;
        channelData->currentPdo    = currentPdoWord;
        channelData->currentRdo    = currentRdoWord;

        channelData->contractActive       = CYPD3177_isContractActive(pdStatusWord);
        channelData->negotiatedVoltage_mV = CYPD3177_negotiatedVoltage_mV(currentPdoWord);
        channelData->negotiatedCurrent_mA = CYPD3177_negotiatedCurrent_mA(currentRdoWord);
        channelData->busVoltage_mV        = CYPD3177_busVoltage_mV(busVoltage);
    }
    else
    {
        // A partial read is not a valid snapshot: keep the last-good cache
        // rather than expose a half-updated one.
        channelData->present = false;
    }
}

/* Public Function Definitions */

// [impl->fw~pd_005~1]
bool dev_CYPD3177_init(const dev_CYPD3177_config_S * const config)
{
    bool success = false;
    if ((config != NULL) && (config->channels != NULL) && (config->numChannels == DEV_CYPD3177_CHANNEL_COUNT))
    {
        bool channelsValid = true;
        for (dev_CYPD3177_channel_E channel = (dev_CYPD3177_channel_E)0U; channel < DEV_CYPD3177_CHANNEL_COUNT; channel++)
        {
            channelsValid &= (config->channels[channel].ioDevice < IO_I2C_DEVICE_COUNT);
        }

        if (channelsValid)
        {
            data->config = config;
            // Establish a known runtime state so a re-init can't inherit stale
            // status from a previous config.
            for (dev_CYPD3177_channel_E channel = (dev_CYPD3177_channel_E)0U; channel < DEV_CYPD3177_CHANNEL_COUNT; channel++)
            {
                dev_CYPD3177_private_clear(&data->channels[channel]);
            }
            success = true;
        }
    }
    return success;
}

void dev_CYPD3177_run200ms(void)
{
    if (data->config != NULL)
    {
        for (dev_CYPD3177_channel_E channel = (dev_CYPD3177_channel_E)0U; channel < DEV_CYPD3177_CHANNEL_COUNT; channel++)
        {
            dev_CYPD3177_private_fetch(channel);
        }
    }
}

// [impl->fw~pd_008~1]
bool dev_CYPD3177_isPresent(dev_CYPD3177_channel_E channel)
{
    bool present = false;
    if ((data->config != NULL) && (channel < DEV_CYPD3177_CHANNEL_COUNT))
    {
        present = data->channels[channel].present;
    }
    return present;
}

// [impl->fw~pd_007~1]
bool dev_CYPD3177_isContractActive(dev_CYPD3177_channel_E channel)
{
    bool active = false;
    if ((data->config != NULL) && (channel < DEV_CYPD3177_CHANNEL_COUNT))
    {
        active = data->channels[channel].contractActive;
    }
    return active;
}

// [impl->fw~pd_007~1]
uint32_t dev_CYPD3177_negotiatedVoltage_mV(dev_CYPD3177_channel_E channel)
{
    uint32_t voltage_mV = 0U;
    if ((data->config != NULL) && (channel < DEV_CYPD3177_CHANNEL_COUNT))
    {
        voltage_mV = data->channels[channel].negotiatedVoltage_mV;
    }
    return voltage_mV;
}

// [impl->fw~pd_007~1]
uint32_t dev_CYPD3177_negotiatedCurrent_mA(dev_CYPD3177_channel_E channel)
{
    uint32_t current_mA = 0U;
    if ((data->config != NULL) && (channel < DEV_CYPD3177_CHANNEL_COUNT))
    {
        current_mA = data->channels[channel].negotiatedCurrent_mA;
    }
    return current_mA;
}

// [impl->fw~pd_007~1]
uint32_t dev_CYPD3177_busVoltage_mV(dev_CYPD3177_channel_E channel)
{
    uint32_t voltage_mV = 0U;
    if ((data->config != NULL) && (channel < DEV_CYPD3177_CHANNEL_COUNT))
    {
        voltage_mV = data->channels[channel].busVoltage_mV;
    }
    return voltage_mV;
}

// [impl->fw~pd_007~1]
bool dev_CYPD3177_getSnapshot(dev_CYPD3177_channel_E channel, dev_CYPD3177_snapshot_S * const snapshot)
{
    bool ret = false;
    if (snapshot != NULL)
    {
        if ((data->config != NULL) && (channel < DEV_CYPD3177_CHANNEL_COUNT))
        {
            *snapshot = data->channels[channel];
            ret = true;
        }
        else
        {
            dev_CYPD3177_private_clear(snapshot);
        }
    }
    return ret;
}
