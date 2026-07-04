#ifndef HW_OPAMP_SIM_H
#define HW_OPAMP_SIM_H

/* Includes */
#include "lib_types.h"
#include "HW_OPAMP.h"

/* SIL inspection / control API — native sim target only. */

// Reset all sim state (uninitialize the driver, clear stored input
// voltages). Call between tests for isolation.
void HW_OPAMP_sim_reset(void);

// Drive the analog voltage present at a channel's input pin. Ignored for
// an out-of-range channel.
void HW_OPAMP_sim_setInputVolts(HW_OPAMP_channels_E channel, float32_t volts);

// Read the voltage the channel's amplifier drives onto its internal ADC
// input: the configured input voltage multiplied by the channel's
// configured gain. Returns false if not initialized, channel out of
// range, or out is NULL.
bool HW_OPAMP_sim_getOutputVolts(HW_OPAMP_channels_E channel, float32_t * const out);

#endif // HW_OPAMP_SIM_H
