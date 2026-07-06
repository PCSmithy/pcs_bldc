#pragma once

// Test-local timer-channel seam. IO_PWM addresses phases by HW_TIM channel; two
// channels let the suite exercise a valid bridge timer plus out-of-range guards.
typedef enum
{
    HW_TIM_CHANNEL_1,
    HW_TIM_CHANNEL_2,
    HW_TIM_CHANNEL_COUNT,
} HW_TIM_channels_E;
