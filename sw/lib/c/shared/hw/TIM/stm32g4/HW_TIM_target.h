#pragma once

// Target-specific half of HW_TIM; reached via HW_TIM.h.

/* Includes */
#include "lib_types.h"
#include "stm32g4xx_hal.h"

#include "HW_TIM_channels.h"

/* Defines */

// Break inputs per timer peripheral. Advanced timers (TIM1/TIM8/TIM20) have BRK
// and BRK2; breakInputs[] is indexed densely by array position, each naming its
// TIM_BREAKINPUT_* id.
#define HW_TIM_BREAK_INPUTS_PER_PERIPHERAL  (2U)

/* Typedefs */

// One break-input source (BRK or BRK2). The break/dead-time block
// (configureBreakDeadTime) enables the break function and its polarity;
// this selects and arms the input source feeding it.
typedef struct
{
    bool                          enabled;
    uint32_t                      breakInput;   // TIM_BREAKINPUT_BRK / _BRK2
    TIMEx_BreakInputConfigTypeDef config;       // Source, Enable, Polarity
} HW_TIM_breakInputConfig_S;

// One timer peripheral.
//
// HW_TIM_init takes htim.Init (Prescaler, Period, CounterMode, ClockDivision,
// RepetitionCounter, AutoReloadPreload) as-is, selects HAL_TIM_PWM_Init when
// any logical channel on the peripheral is a PWM output-compare unit (else
// HAL_TIM_Base_Init), configures the internal clock source, applies the
// break/dead-time block (when configureBreakDeadTime) and the TRGO master
// config (when configureTrgo), then starts the counter with every output
// disabled. The peripheral's output-compare units are supplied by the logical
// channels (HW_TIM_channelConfig_S) that name it.
typedef struct
{
    TIM_HandleTypeDef htim;

    bool                           configureBreakDeadTime;
    TIM_BreakDeadTimeConfigTypeDef breakDeadTime;

    // Break-input sources (BRK/BRK2). Applied after the break/dead-time
    // block; disabled entries are skipped.
    HW_TIM_breakInputConfig_S breakInputs[HW_TIM_BREAK_INPUTS_PER_PERIPHERAL];

    bool                    configureTrgo;
    TIM_MasterConfigTypeDef master;
} HW_TIM_peripheralConfig_S;

// One logical channel: an output-compare unit on a named peripheral.
//
// Library-managed oc fields (overwritten by HW_TIM_init): (none) — OCMode,
// Pulse (initial compare), OCPolarity, OCNPolarity, OCIdleState, OCNIdleState
// are taken as-is. .complementary drives the CHxN output as well (advanced
// timers only).
typedef struct
{
    HW_TIM_peripheral_E  peripheral;    // peripheral this channel lives on
    HW_TIM_channelRole_E role;          // HW_TIM_ROLE_OUTPUT_COMPARE
    uint8_t              ocUnit;        // dense OC-unit index 0..3
    bool                 complementary; // also drive CHxN
    uint32_t             channel;       // TIM_CHANNEL_1 .. TIM_CHANNEL_4
    TIM_OC_InitTypeDef   oc;
} HW_TIM_channelConfig_S;
