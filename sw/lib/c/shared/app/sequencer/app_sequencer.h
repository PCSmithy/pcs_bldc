#pragma once

/* Includes */
#include "lib_types.h"

#include "app_motorControl.h"
#include "app_sequencer_channels.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Typedefs */

// One kind per motor-control target, and the stop.
typedef enum
{
    APP_SEQUENCER_KIND_VELOCITY,   // rad/s, rate limit in rad/s^2
    APP_SEQUENCER_KIND_TORQUE,     // Nm, rate limit in Nm/s
    APP_SEQUENCER_KIND_STOP,       // MODE_OFF requested; ends the sequence
    APP_SEQUENCER_KIND_POSITION,   // rad, with its move limits
    APP_SEQUENCER_KIND_COUNT,
} app_sequencer_kind_E;

// A speed or torque target; the rate limit per second in its unit, zero steps (fw~mc_027).
typedef struct
{
    float32_t value;
    float32_t rateLimit;
} app_sequencer_target_S;

// A position move (fw~mc_030); zero limits step at the maximum velocity.
typedef struct
{
    float32_t position_rad;
    float32_t velocityLimit_radPerSec;
    float32_t accelLimit_radPerSec2;
    float32_t velocityAtPoint_radPerSec;
} app_sequencer_move_S;

typedef struct
{
    uint32_t             t_us;   // from the sequence's start
    app_sequencer_kind_E kind;   // names the arm in use; a stop uses neither
    union
    {
        app_sequencer_target_S target;
        app_sequencer_move_S   move;
    };
} app_sequencer_setpoint_S;

// Why append refused a setpoint.
typedef enum
{
    APP_SEQUENCER_APPEND_OK,
    APP_SEQUENCER_APPEND_NOT_STAGING,   // no begin, or a commit awaits its pass
    APP_SEQUENCER_APPEND_FULL,
    APP_SEQUENCER_APPEND_OUT_OF_ORDER,  // its time precedes the setpoint before it
    APP_SEQUENCER_APPEND_INVALID,       // no setpoint, or an unknown kind
} app_sequencer_append_E;

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
    bool     running;      // a committed sequence has setpoints left to write
    uint32_t elapsed_ms;   // since the running (or last) sequence's start; 0 before it starts
} app_sequencer_status_S;

/* Public Function Declarations */

bool app_sequencer_init(const app_sequencer_config_S * const config);

// The 1 ms pass (fw~ops_001, fw~ops_002). Runs before the motor control's pass
// so a write lands the same millisecond.
void app_sequencer_run1ms(void);

// Staging, from one task only: begin empties the staged table (false while a
// commit awaits its pass), append adds to it in time order, commit has it
// replace the running sequence on the next pass. An empty commit stops.
bool app_sequencer_begin(app_sequencer_channel_E channel);
app_sequencer_append_E app_sequencer_append(app_sequencer_channel_E channel, const app_sequencer_setpoint_S * const setpoint);
bool app_sequencer_commit(app_sequencer_channel_E channel);

bool app_sequencer_getStatus(app_sequencer_channel_E channel, app_sequencer_status_S * const status);

#ifdef __cplusplus
}
#endif
