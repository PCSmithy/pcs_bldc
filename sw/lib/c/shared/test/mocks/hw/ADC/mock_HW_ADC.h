#pragma once

#include "lib_types.h"
#include "HW_ADC_channels.h"

// Test controls for the mocked HW_ADC. It holds a per-(channel, IN#) volts
// value and a "readable" flag; HW_ADC_getVolts returns the stored volts only
// where a value has been set, mirroring the real driver's "input not enabled ->
// false" behavior so the bridge's read-failure path can be exercised.

// Clear all stored readings; every input starts unreadable (getVolts fails).
void mock_HW_ADC_reset(void);

// Make one (channel, IN#) readable and set the volts HW_ADC_getVolts returns.
void mock_HW_ADC_setVolts(HW_ADC_channels_E channel, uint8_t inputIndex, float32_t volts);
