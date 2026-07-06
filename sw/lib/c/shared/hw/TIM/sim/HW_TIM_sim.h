#pragma once

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
// channel's master output enable, and the counter-vs-compare comparison. A
// disabled unit or a cleared master output enable yields the configured
// inactive level.
uint32_t HW_TIM_sim_getOutputLevel(HW_TIM_channels_E channel, uint8_t ocUnit);

// Effective complementary-output level (0/1). Antiphase to the primary while
// running; inactive when the unit is disabled or the master output enable is
// clear.
uint32_t HW_TIM_sim_getComplementaryLevel(HW_TIM_channels_E channel, uint8_t ocUnit);

// Configured dead-time generator ticks for a channel (0 when none).
uint32_t HW_TIM_sim_getDeadTime(HW_TIM_channels_E channel);

// Number of trigger-output events tallied since reset (or the last
// HW_TIM_sim_clearTriggers).
uint32_t HW_TIM_sim_getTriggerCount(HW_TIM_channels_E channel);
void HW_TIM_sim_clearTriggers(HW_TIM_channels_E channel);

// Assert the channel's break input. An assertion clears the master output
// enable latch (forcing every output inactive); it stays clear after release
// until HW_TIM_setMainOutputEnabled sets it again. Deassert (asserted=false)
// is a no-op, modelling a break that leaves outputs latched off.
void HW_TIM_sim_assertBreak(HW_TIM_channels_E channel, bool asserted);

