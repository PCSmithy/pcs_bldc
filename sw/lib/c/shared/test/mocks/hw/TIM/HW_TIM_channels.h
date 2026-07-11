#pragma once

// Test-local HW_TIM seams. Two peripherals plus a channel on the second one let
// the suite exercise the "all three phases share one peripheral" rule and the
// out-of-range guards.
typedef enum
{
    HW_TIM_PERIPHERAL_1,
    HW_TIM_PERIPHERAL_2,
    HW_TIM_PERIPHERAL_COUNT,
} HW_TIM_peripheral_E;

typedef enum
{
    HW_TIM_CHANNEL_PWM_U,   // peripheral 1
    HW_TIM_CHANNEL_PWM_V,   // peripheral 1
    HW_TIM_CHANNEL_PWM_W,   // peripheral 1
    HW_TIM_CHANNEL_OTHER,   // peripheral 2 (foreign to the bridge)
    HW_TIM_CHANNEL_COUNT,
} HW_TIM_channels_E;
