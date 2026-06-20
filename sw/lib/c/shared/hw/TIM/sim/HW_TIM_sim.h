#ifndef HW_TIM_SIM_H
#define HW_TIM_SIM_H

/* Includes */
#include "lib_types.h"
#include "HW_TIM.h"

/* SIL inspection / control API — native sim target only. */

// Reset all sim state (uninitialize the driver, clear counters/outputs).
// Call between tests for isolation.
void HW_TIM_sim_reset(void);

// Advance a channel's counter by `ticks` prescaled steps, following its
// configured direction and wrapping at its period. Tallies trigger-output
// events (per the channel's configured TRGO source) seen during the
// advance — the SIL stand-in for time passing on hardware.
void HW_TIM_sim_advance(HW_TIM_channels_E channel, uint32_t ticks);

// True when an output-compare unit's output is enabled
// (HW_TIM_setOutputEnabled).
bool HW_TIM_sim_getOutputEnabled(HW_TIM_channels_E channel, uint8_t ocUnit);

// Effective primary-output level (0/1) given the unit's enable state, the
// break input, and the counter-vs-compare comparison. A disabled unit or
// an asserted break yields the configured inactive level.
uint32_t HW_TIM_sim_getOutputLevel(HW_TIM_channels_E channel, uint8_t ocUnit);

// Effective complementary-output level (0/1). Antiphase to the primary
// while running; inactive when the unit is disabled or the break is
// asserted.
uint32_t HW_TIM_sim_getComplementaryLevel(HW_TIM_channels_E channel, uint8_t ocUnit);

// Configured dead-time generator ticks for a channel (0 when none).
uint32_t HW_TIM_sim_getDeadTime(HW_TIM_channels_E channel);

// Number of trigger-output events tallied since reset (or the last
// HW_TIM_sim_clearTriggers).
uint32_t HW_TIM_sim_getTriggerCount(HW_TIM_channels_E channel);
void HW_TIM_sim_clearTriggers(HW_TIM_channels_E channel);

// Assert or deassert the channel's break input. While asserted, every
// output of the channel is forced to its inactive level.
void HW_TIM_sim_assertBreak(HW_TIM_channels_E channel, bool asserted);

#endif // HW_TIM_SIM_H
