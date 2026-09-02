#pragma once

#include "lib_types.h"
#include "HW_ADC.h"

// Test controls for the mocked HW_ADC. It holds a per-(channel, IN#) volts
// value and a "readable" flag on each sequence; getVolts / getInjectedVolts
// return the stored volts only where a value has been set, mirroring the real
// driver's "input not enabled -> false" behavior so a consumer's read-failure
// path can be exercised. Registered injected callbacks are captured so a test
// can fire a completion by hand instead of running an ISR.

// Clear all stored readings and registrations; every input starts unreadable.
void mock_HW_ADC_reset(void);

// Make one (channel, IN#) readable and set the volts HW_ADC_getVolts returns.
void mock_HW_ADC_setVolts(HW_ADC_channels_E channel, uint8_t inputIndex, float32_t volts);

// Same for one injected sequence slot (indexed by sequence position, not IN#).
void mock_HW_ADC_setInjectedVolts(HW_ADC_channels_E channel, uint8_t injectedIndex, float32_t volts);

// Make every later HW_ADC_registerInjectedCallback on this channel fail.
void mock_HW_ADC_failRegistration(HW_ADC_channels_E channel);

// How many callbacks were registered on this channel since reset.
uint32_t mock_HW_ADC_getRegistrationCount(HW_ADC_channels_E channel);

// Invoke the channel's registered callback, as the completion ISR would.
void mock_HW_ADC_fireInjected(HW_ADC_channels_E channel, HW_ADC_conversionStatus_E status);

// Set the dispatch sequence later fires run under. Reset leaves it at 1, so
