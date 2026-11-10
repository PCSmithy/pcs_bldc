#pragma once

/* Includes */
#include "lib_types.h"

#include "app_motorControl.h"
#include "app_sequencer_channels.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Typedefs */

// One kind per motor-control target, and the stop; position joins here.
typedef enum
{
    APP_SEQUENCER_KIND_VELOCITY,   // rad/s, rate limit in rad/s^2
    APP_SEQUENCER_KIND_TORQUE,     // Nm, rate limit in Nm/s
    APP_SEQUENCER_KIND_STOP,       // MODE_OFF requested; value and rate limit unused
    APP_SEQUENCER_KIND_COUNT,
} app_sequencer_kind_E;

typedef struct
{
    uint32_t             t_us;        // from the sequence's start
    app_sequencer_kind_E kind;
    float32_t            value;
    float32_t            rateLimit;   // per second in the kind's unit; zero steps (fw~mc_027)
} app_sequencer_setpoint_S;

typedef struct
{
    app_motorControl_channel_E motor;     // the channel the setpoints are written to
    app_sequencer_setpoint_S * storage;   // 2 * capacity entries: the running table and the staged one
    uint32_t                   capacity;
} app_sequencer_channelConfig_S;

typedef struct
{
    const app_sequencer_channelConfig_S * channels;
    size_t numChannels;
} app_sequencer_config_S;

typedef struct
{
    bool     running;      // setpoints remain to be written
    uint32_t elapsed_ms;   // milliseconds run since the running (or last) sequence's start
} app_sequencer_status_S;

/* Public Function Declarations */

bool app_sequencer_init(const app_sequencer_config_S * const config);

// The 1 ms pass: starts a committed sequence, writes the setpoints due, drops
// the rest on a latched fault (fw~ops_001, fw~ops_002). Runs before the motor
// control's pass so a write lands the same millisecond.
void app_sequencer_run1ms(void);

// Staging a sequence, from one task only: begin empties the staged table,
// append adds a setpoint (false at capacity or before begin), commit hands
// the table over to start on the next pass, replacing the running sequence.
// An empty commit stops. begin is false while a commit awaits its pass (at
// most 1 ms): that sequence was accepted and plays; the caller retries.
bool app_sequencer_begin(app_sequencer_channel_E channel);
bool app_sequencer_append(app_sequencer_channel_E channel, const app_sequencer_setpoint_S * const setpoint);
bool app_sequencer_commit(app_sequencer_channel_E channel);

bool app_sequencer_getStatus(app_sequencer_channel_E channel, app_sequencer_status_S * const status);

#ifdef __cplusplus
}
#endif
