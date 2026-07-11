#pragma once

// Minimal mock of the HW_ADC public header — only the surface IO_bridge uses
// (getVolts, on the regular sequence). The implementation lives in
// mock_HW_ADC.c, driven by the controls in mock_HW_ADC.h.

#include "lib_types.h"
#include "HW_ADC_channels.h"

bool HW_ADC_getVolts(HW_ADC_channels_E channel, uint8_t inputIndex, float32_t * const out);
