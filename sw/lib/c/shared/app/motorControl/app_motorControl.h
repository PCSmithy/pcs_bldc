#pragma once

/* Includes */
#include "lib_types.h"

#include "IO_bridge.h"
#include "IO_AS5048.h"

#include "dev_gateDriver.h"

#include "app_motorControl_channels.h"

/* Defines */


/* Typedefs */

typedef enum
{
    APP_MOTORCONTROL_MODE_OFF,
    APP_MOTORCONTROL_MODE_SIX_STEP_TRAP,
    // APP_MOTORCONTROL_MODE_VF_SINUSOIDAL, // TODO
    // APP_MOTORCONTROL_MODE_FOC, // TODO
    APP_MOTORCONTROL_MODE_COUNT,
} app_motorControl_mode_E;

// Coarse bridge state for the operator display (fw~mc_009): DISABLED (bridge
// dark), ENABLED (bridge driving), or FAULTED (a latched fault holds it off).
typedef enum
{
    APP_MOTORCONTROL_STATE_DISABLED,
    APP_MOTORCONTROL_STATE_ENABLED,
    APP_MOTORCONTROL_STATE_FAULTED,
    APP_MOTORCONTROL_STATE_COUNT,
} app_motorControl_state_E;


typedef struct
{
    dev_gateDriver_channel_E gateDriver;
    IO_bridge_channel_E bridge;

    float32_t maxVelocity_radPerSec;

    IO_AS5048_channel_E encoder; // TODO - abstract IO_AS5048 into a multi-encoder dev_encoder

    uint8_t motorPolePairs;
} app_motorControl_channelConfig_S;

typedef struct
{
    const app_motorControl_channelConfig_S * channels;
    size_t numChannels;
} app_motorControl_config_S;

// Telemetry/diagnostic view of one channel's live state, and the source the
// operator display (app_rgbLedRing) reads for state + speedometer indication.
typedef struct
{
    app_motorControl_mode_E  mode;
    app_motorControl_state_E state;                        // disabled / enabled / faulted
    bool                     isAligned;
    float32_t                magneticAngle_rad;            // rotor electrical angle
    float32_t                velocitySetpoint_radPerSec;   // signed commanded speed target
} app_motorControl_snapshot_S;

/* Public Function Declarations */

bool app_motorControl_init(const app_motorControl_config_S * const config);

// Copy out the channel's live mode/alignment/electrical-angle state. Returns
// false for an out-of-range channel, a NULL destination, or an uninitialized
// module, leaving the destination unchanged.
bool app_motorControl_getSnapshot(app_motorControl_channel_E channel, app_motorControl_snapshot_S * const snapshot);

void app_motorControl_run1ms(void);

void app_motorControl_setVelocity(app_motorControl_channel_E channel, float32_t velocity_radPerSec);
void app_motorControl_setMode(app_motorControl_channel_E channel, app_motorControl_mode_E mode);

// Release a latched overcurrent fault (fw~safety_001). The drive stays disabled
// until re-enabled; a no-op for an out-of-range channel or an uninitialized
// module. This is the fault-clear action referenced by the trip latch — driven
// on-device by the user button's fault-clear gesture (fw~mc_007).
void app_motorControl_clearFault(app_motorControl_channel_E channel);
