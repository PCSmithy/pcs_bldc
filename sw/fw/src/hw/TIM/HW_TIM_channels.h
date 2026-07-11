#pragma once

typedef enum
{
    HW_TIM_PERIPHERAL_1,       // TIM1: 3-phase complementary motor PWM
    HW_TIM_PERIPHERAL_2,       // TIM2: 1 us free-running time base
    HW_TIM_PERIPHERAL_COUNT,
} HW_TIM_peripheral_E;

typedef enum
{
    HW_TIM_CHANNEL_PWM_U,      // TIM1 CH1: phase-U complementary PWM
    HW_TIM_CHANNEL_PWM_V,      // TIM1 CH2: phase-V complementary PWM
    HW_TIM_CHANNEL_PWM_W,      // TIM1 CH3: phase-W complementary PWM
    HW_TIM_CHANNEL_COUNT,
} HW_TIM_channels_E;
