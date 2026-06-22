#ifndef DEV_SWITCH_CHANNELS_H
#define DEV_SWITCH_CHANNELS_H

// Test-local channel seam (mirrors the project-provided header). Two buttons
// so per-channel debounce, addressing, and both active-level polarities can be
// exercised independently.
typedef enum
{
    DEV_SWITCH_CHANNEL_BTN_A,
    DEV_SWITCH_CHANNEL_BTN_B,
    DEV_SWITCH_CHANNEL_COUNT,
} DEV_switch_channel_E;

#endif // DEV_SWITCH_CHANNELS_H
