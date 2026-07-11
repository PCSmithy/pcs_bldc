#pragma once

// Minimal mock of the HW_TIM public header — only the surface IO_bridge uses.
// The implementations live in mock_HW_TIM.c, driven by the controls in
// mock_HW_TIM.h.

#include "lib_types.h"
#include "HW_TIM_channels.h"

bool HW_TIM_getPeripheral(HW_TIM_channels_E channel, HW_TIM_peripheral_E * const out);
bool HW_TIM_getPeriod(HW_TIM_channels_E channel, uint32_t * const out);
bool HW_TIM_setCompare(HW_TIM_channels_E channel, uint32_t counts);
bool HW_TIM_setOutputEnabled(HW_TIM_channels_E channel, bool enabled);
bool HW_TIM_setMainOutputEnabled(HW_TIM_peripheral_E peripheral, bool enabled);
bool HW_TIM_getMainOutputEnabled(HW_TIM_peripheral_E peripheral, bool * const enabled);
bool HW_TIM_clearBreakFlags(HW_TIM_peripheral_E peripheral);
