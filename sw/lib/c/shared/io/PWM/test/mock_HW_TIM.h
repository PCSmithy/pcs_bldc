#pragma once

#include "lib_types.h"
#include "HW_TIM.h"

// Test controls for the mocked HW_TIM. It records the compare and output-enable
// state IO_PWM sets per (channel, ocUnit), holds a per-channel MOE latch, and
// reports a configurable period so duty math is checked without a real ARR.

// Clear all recorded state; set every channel's period to `period`; MOE off.
void mock_HW_TIM_reset(uint32_t period);

// Override one channel's reported period.
void mock_HW_TIM_setPeriod(HW_TIM_channels_E channel, uint32_t period);

// Model a break event: clears the channel's MOE latch (as hardware does with
// AutomaticOutput disabled), and it stays clear until set again.
void mock_HW_TIM_assertBreak(HW_TIM_channels_E channel);

// Inspect recorded state.
uint32_t mock_HW_TIM_getCompare(HW_TIM_channels_E channel, uint8_t ocUnit);
bool     mock_HW_TIM_getOutputEnabled(HW_TIM_channels_E channel, uint8_t ocUnit);
bool     mock_HW_TIM_getMoe(HW_TIM_channels_E channel);
