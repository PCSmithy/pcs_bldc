#pragma once

/* Defines */
// Setpoints one sequence holds: sized for one frame's worth (IO_COBSFRAME_MAX_PAYLOAD, 448 B).
#define APP_SEQUENCER_CAPACITY (24U)

/* Typedefs */

typedef enum
{
    APP_SEQUENCER_CHANNEL_MAIN,
    APP_SEQUENCER_CHANNEL_COUNT,
} app_sequencer_channel_E;
