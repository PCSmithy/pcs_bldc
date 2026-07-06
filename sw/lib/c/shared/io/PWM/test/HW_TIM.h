#pragma once

// Minimal mock of the HW_TIM public header — only the surface IO_PWM uses.
// The implementations live in mock_HW_TIM.c, driven by the controls in
// mock_HW_TIM.h.

#include "lib_types.h"
#include "HW_TIM_channels.h"

// Output-compare units per timer (mirrors the real HW_TIM define).
#define HW_TIM_OC_UNITS_PER_CHANNEL  (4U)

bool HW_TIM_getPeriod(HW_TIM_channels_E channel, uint32_t * const out);
bool HW_TIM_setCompare(HW_TIM_channels_E channel, uint8_t ocUnit, uint32_t counts);
bool HW_TIM_setOutputEnabled(HW_TIM_channels_E channel, uint8_t ocUnit, bool enabled);
bool HW_TIM_setMainOutputEnabled(HW_TIM_channels_E channel, bool enabled);
bool HW_TIM_getMainOutputEnabled(HW_TIM_channels_E channel, bool * const enabled);
