#pragma once

/* Includes */
#include "lib_types.h"
#include "HW_ADC.h"

/* SIL inspection / control API — native sim target only. */

// Reset all sim state (uninitialize the driver, clear stored counts). Call
// between tests for isolation.
void HW_ADC_sim_reset(void);

// True when the channel's multimode configuration was applied during init
// (the channel was flagged configureMultimode and init succeeded).
bool HW_ADC_sim_getMultimodeApplied(HW_ADC_channels_E channel);

// Inject a stalled conversion on a channel. While set, each _run1ms pass
// leaves the channel's counts untouched and reports
// HW_ADC_CONVERSION_STATUS_FAULT — the SIL stand-in for a poll timeout on
// hardware. Clear it to resume normal sampling.
void HW_ADC_sim_setConversionStall(HW_ADC_channels_E channel, bool stall);

