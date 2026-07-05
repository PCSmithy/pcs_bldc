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

// Everything the driver fetches from the chip: the raw HPI register words
// (bytes assembled little-endian, exactly as read) plus the decoded values the
// scalar accessors expose. `present` follows the rules of dev_CYPD3177_isPresent;
// the other fields retain their last-good values across a failed fetch.
typedef struct
{
    bool     present;

    // Raw register words, as last fetched.
    uint8_t  deviceMode;
    uint16_t siliconId;
    uint32_t pdStatus;
    uint32_t typeCStatus;
    uint8_t  busVoltageRaw;   // 100 mV per LSB
    uint32_t currentPdo;
    uint32_t currentRdo;

    // Decoded engineering values (lib_CYPD3177).
    bool     contractActive;
    uint32_t negotiatedVoltage_mV;
    uint32_t negotiatedCurrent_mA;
    uint32_t busVoltage_mV;
} dev_CYPD3177_snapshot_S;

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

// Copy the channel's complete cached state (raw words + decoded values) into
// `snapshot`. Returns false — with `snapshot` zeroed — for an out-of-range
// channel, before init, or a NULL snapshot. The copy is unlocked: a fetch on
// another task may land between fields, so treat it as telemetry-grade.
bool dev_CYPD3177_getSnapshot(dev_CYPD3177_channel_E channel, dev_CYPD3177_snapshot_S * const snapshot);
