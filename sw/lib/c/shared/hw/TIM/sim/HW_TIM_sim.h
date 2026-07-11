#pragma once

/* Includes */
#include "lib_types.h"
#include "HW_TIM.h"

/* SIL inspection / control API — native sim target only. */

// Reset all sim state (uninitialize the driver, clear counters/outputs).
// Call between tests for isolation.
void HW_TIM_sim_reset(void);

// Advance a peripheral's counter by `ticks` prescaled steps, following its
// configured direction and wrapping at its period. Tallies trigger-output
// events (per the peripheral's configured TRGO source) seen during the
// advance — the SIL stand-in for time passing on hardware.
void HW_TIM_sim_advance(HW_TIM_peripheral_E peripheral, uint32_t ticks);

// True when a logical channel's output is enabled (HW_TIM_setOutputEnabled).
bool HW_TIM_sim_getOutputEnabled(HW_TIM_channels_E channel);

// Effective primary-output level (0/1) of a logical channel given its enable
// state, its peripheral's master output enable, and the counter-vs-compare
// comparison. A disabled channel or a cleared master output enable yields the
// configured inactive level.
uint32_t HW_TIM_sim_getOutputLevel(HW_TIM_channels_E channel);

// Effective complementary-output level (0/1) of a logical channel. Antiphase to
// the primary while running; inactive when the channel is disabled or the
// master output enable is clear.
uint32_t HW_TIM_sim_getComplementaryLevel(HW_TIM_channels_E channel);

// Configured dead-time generator ticks for a peripheral (0 when none).
uint32_t HW_TIM_sim_getDeadTime(HW_TIM_peripheral_E peripheral);

// Number of trigger-output events tallied since reset (or the last
// HW_TIM_sim_clearTriggers).
uint32_t HW_TIM_sim_getTriggerCount(HW_TIM_peripheral_E peripheral);
void HW_TIM_sim_clearTriggers(HW_TIM_peripheral_E peripheral);

// Assert a peripheral's break input. An assertion clears the master output
// enable latch (forcing every output inactive); it stays clear after release
// until HW_TIM_setMainOutputEnabled sets it again. Deassert (asserted=false)
// is a no-op, modelling a break that leaves outputs latched off.
void HW_TIM_sim_assertBreak(HW_TIM_peripheral_E peripheral, bool asserted);
