#pragma once

/* Includes */
#include "lib_types.h"
#include "IO_i2c.h"
#include "dev_gateDriver_channels.h"

/* Defines */

// LOCK register values: unlock satisfies LOCK == ~NLOCK (fields [3:0]/[7:4]);
// relock violates it.
#define DEV_GATEDRIVER_LOCK_UNLOCK (0x0FU)
#define DEV_GATEDRIVER_LOCK_RELOCK (0x00U)

// CLEAR command register: all bits set clears every latched fault; the device
// rejects any other value.
#define DEV_GATEDRIVER_CLEAR_ALL   (0xFFU)

// STATUS register bit positions.
#define DEV_GATEDRIVER_STATUS_VCC_UVLO_SHIFT (0U)
#define DEV_GATEDRIVER_STATUS_THSD_SHIFT     (1U)
#define DEV_GATEDRIVER_STATUS_VDS_P_SHIFT    (2U)
#define DEV_GATEDRIVER_STATUS_RESET_SHIFT    (3U)
#define DEV_GATEDRIVER_STATUS_LOCK_SHIFT     (7U)

/* Typedefs */

// STSPIN32G4 gate-driver register offsets (8-bit) addressed over I2C.
typedef enum
{
    DEV_GATEDRIVER_REG_POWMNG = 0x01,
    DEV_GATEDRIVER_REG_LOGIC  = 0x02,
    DEV_GATEDRIVER_REG_READY  = 0x07,
    DEV_GATEDRIVER_REG_NFAULT = 0x08,
    DEV_GATEDRIVER_REG_CLEAR  = 0x09,
    DEV_GATEDRIVER_REG_LOCK   = 0x0B,
    DEV_GATEDRIVER_REG_STATUS = 0x80,
} dev_gateDriver_register_E;

typedef struct
{
    IO_i2c_device_E ioDevice;

    // Register values the configuration sequence programs and verifies.
    uint8_t powmng;
    uint8_t logic;
    uint8_t ready;
    uint8_t nfault;
} dev_gateDriver_channelConfig_S;

typedef struct
{
    const dev_gateDriver_channelConfig_S * channels;
    size_t numChannels;
} dev_gateDriver_config_S;

// The channel's complete cached state. `configured` reports the configuration
// sequence's completion; `statusOk` reports the most recent STATUS read; the
// decoded flags retain their last successfully read values across a failed
// read.
typedef struct
{
    bool    configured;
    bool    statusOk;
    uint8_t statusRaw;

    // Decoded STATUS flags.
    bool locked;
    bool resetLatched;
    bool vdsProtection;
    bool thermalShutdown;
    bool vccUndervoltage;
} dev_gateDriver_snapshot_S;

/* Public Function Declarations */

bool dev_gateDriver_init(const dev_gateDriver_config_S * const config);

void dev_gateDriver_run200ms(void);

// Return the channel's cached state, or a safe default (false / zeroed
// snapshot) for an out-of-range channel or before init.
bool dev_gateDriver_isConfigured(dev_gateDriver_channel_E channel);
bool dev_gateDriver_getSnapshot(dev_gateDriver_channel_E channel,
                                dev_gateDriver_snapshot_S * const snapshot);

// The go/no-go gate for driving the power stage: configured, the most recent
// STATUS read succeeded, and the cached STATUS reports locked (unlocked
// protected registers force the gate outputs low) with every fault flag clear.
bool dev_gateDriver_isOperational(dev_gateDriver_channel_E channel);

// Clear the device's latched faults (CLEAR = 0xFF). A deliberate consumer
// action — the driver never clears runtime faults on its own.
bool dev_gateDriver_clearFaults(dev_gateDriver_channel_E channel);
