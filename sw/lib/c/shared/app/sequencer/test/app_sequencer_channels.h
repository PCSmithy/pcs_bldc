#pragma once

// Test-local sequencer channel seam (mirrors the board's one channel and capacity).
#define APP_SEQUENCER_CAPACITY (24U)

typedef enum
{
    APP_SEQUENCER_CHANNEL_MAIN,
    APP_SEQUENCER_CHANNEL_COUNT,
} app_sequencer_channel_E;
