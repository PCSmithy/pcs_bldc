#ifndef DEV_SWITCH_CHANNELS_H
#define DEV_SWITCH_CHANNELS_H

// Test-local channel seam (mirrors the project-provided header). Two hardware
// buttons (one per active-level polarity) plus a network-backed switch, so both
// config union variants and per-channel debounce can be exercised independently.
typedef enum
{
    DEV_SWITCH_CHANNEL_BTN_A,    // HW digital-in, active-low
    DEV_SWITCH_CHANNEL_BTN_B,    // HW digital-in, active-high
    DEV_SWITCH_CHANNEL_BTN_NET,  // network-provided state
    DEV_SWITCH_CHANNEL_COUNT,
} dev_switch_channel_E;

#endif // DEV_SWITCH_CHANNELS_H
