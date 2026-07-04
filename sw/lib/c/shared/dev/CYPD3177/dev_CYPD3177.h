#pragma once

/* Includes */
#include "lib_types.h"
#include "IO_i2c.h"
#include "dev_CYPD3177_channels.h"

/* Typedefs */

// Bus, address, and register-offset width live in the IO_i2c device config;
// this layer only selects which device.
typedef struct
{
    IO_i2c_device_E ioDevice;
} dev_CYPD3177_channelConfig_S;

typedef struct
{
    const dev_CYPD3177_channelConfig_S * channels;
    size_t numChannels;
} dev_CYPD3177_config_S;

/* Public Function Declarations */

bool dev_CYPD3177_init(const dev_CYPD3177_config_S * const config);

void dev_CYPD3177_run200ms(void);

// Present when the most recent fetch read every register and DEVICE_MODE read
// back nonzero. False for an out-of-range channel or before init.
bool dev_CYPD3177_isPresent(dev_CYPD3177_channel_E channel);

// Return the channel's last successfully fetched value, or a safe default
// (false / 0) for an out-of-range channel or before init.
bool     dev_CYPD3177_isContractActive   (dev_CYPD3177_channel_E channel);
uint32_t dev_CYPD3177_negotiatedVoltage_mV(dev_CYPD3177_channel_E channel);
uint32_t dev_CYPD3177_negotiatedCurrent_mA(dev_CYPD3177_channel_E channel);
uint32_t dev_CYPD3177_busVoltage_mV      (dev_CYPD3177_channel_E channel);
