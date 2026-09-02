#pragma once

#include "lib_types.h"
#include "HW_TIM.h"

// Test controls for the mocked HW_TIM. It records the compare and output-enable
// state IO_bridge sets per logical channel, holds a per-peripheral MOE latch,
// maps each channel to a peripheral, and reports a configurable period so duty
// math is checked without a real ARR.

// Clear all recorded state; set every channel's period to `period`; map
// PWM_U/V/W to peripheral 1 and OTHER to peripheral 2; MOE off.
void mock_HW_TIM_reset(uint32_t period);

// Override one channel's reported period.
void mock_HW_TIM_setPeriod(HW_TIM_channels_E channel, uint32_t period);

// Override one channel's owning peripheral (for the same-peripheral rule).
void mock_HW_TIM_setPeripheral(HW_TIM_channels_E channel, HW_TIM_peripheral_E peripheral);

// Model a break event: clears the peripheral's MOE latch (as hardware does with
// AutomaticOutput disabled), and it stays clear until set again.
void mock_HW_TIM_assertBreak(HW_TIM_peripheral_E peripheral);

// Drive the free-running time base the injected callback stamps samples with.
void mock_HW_TIM_setCounter(HW_TIM_peripheral_E peripheral, uint32_t counts);

// Force HW_TIM_getCounter to fail, so a caller's no-time-source path is testable.
void mock_HW_TIM_setGetCounterFails(bool fails);

// Inspect recorded state.
uint32_t mock_HW_TIM_getCompare(HW_TIM_channels_E channel);
bool     mock_HW_TIM_getOutputEnabled(HW_TIM_channels_E channel);
bool     mock_HW_TIM_getMoe(HW_TIM_peripheral_E peripheral);
uint32_t mock_HW_TIM_getBreakFlagsClearCount(HW_TIM_peripheral_E peripheral);
