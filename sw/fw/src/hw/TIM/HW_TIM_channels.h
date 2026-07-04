#pragma once

// Logical timer channels on the pcs_bldc board. Each maps to one physical
// STM32G4 timer peripheral, addressed by hardware enumeration.
typedef enum
{
    HW_TIM_CHANNEL_1,       // TIM1: 3-phase complementary motor PWM
    HW_TIM_CHANNEL_2,       // TIM2: 1 us free-running time base
    HW_TIM_CHANNEL_COUNT,
} HW_TIM_channels_E;

