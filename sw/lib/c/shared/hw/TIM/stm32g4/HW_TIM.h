#ifndef HW_TIM_H
#define HW_TIM_H

/* Includes */
#include "lib_types.h"
#include "stm32g4xx_hal.h"
#include "HW_TIM_channels.h"

/* Defines */

// Output-compare units per timer. STM32G4 general/advanced timers expose
// up to CH1..CH4; outputCompare[] is indexed densely 0..3 by array
// position, and each entry names its TIM_CHANNEL_n in .channel.
#define HW_TIM_OC_UNITS_PER_CHANNEL  (4U)

// Break inputs per timer. Advanced timers (TIM1/TIM8/TIM20) have BRK and
// BRK2; breakInputs[] is indexed densely by array position, each naming
// its TIM_BREAKINPUT_* id.
#define HW_TIM_BREAK_INPUTS_PER_CHANNEL  (2U)

/* Typedefs */

// One output-compare unit on a timer (one of CH1..CH4).
//
// Library-managed oc fields (overwritten by HW_TIM_init):
//   (none) — OCMode, Pulse (initial compare), OCPolarity, OCNPolarity,
//   OCIdleState, OCNIdleState are taken as-is from this config.
//
// .complementary drives the CHxN output as well (advanced timers only).
typedef struct
{
    bool               enabled;        // configure + manage this unit's output
    uint32_t           channel;        // TIM_CHANNEL_1 .. TIM_CHANNEL_4
    bool               complementary;  // also drive CHxN
    TIM_OC_InitTypeDef oc;
} HW_TIM_ocConfig_S;

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
// HW_TIM_init takes htim.Init (Prescaler, Period, CounterMode,
// ClockDivision, RepetitionCounter, AutoReloadPreload) as-is, selects
// HAL_TIM_PWM_Init when any output-compare unit is enabled (else
// HAL_TIM_Base_Init), configures the internal clock source, applies each
// enabled output-compare unit, the break/dead-time block (when
// configureBreakDeadTime), and the TRGO master config (when
// configureTrgo), then starts the counter with every output disabled.
typedef struct
{
    TIM_HandleTypeDef htim;

    HW_TIM_ocConfig_S outputCompare[HW_TIM_OC_UNITS_PER_CHANNEL];

    bool                           configureBreakDeadTime;
    TIM_BreakDeadTimeConfigTypeDef breakDeadTime;

    // Break-input sources (BRK/BRK2). Applied after the break/dead-time
    // block; disabled entries are skipped.
    HW_TIM_breakInputConfig_S breakInputs[HW_TIM_BREAK_INPUTS_PER_CHANNEL];

    bool                    configureTrgo;
    TIM_MasterConfigTypeDef master;
} HW_TIM_channelConfig_S;

typedef struct
{
    const HW_TIM_channelConfig_S * channels;
    size_t numChannels;
} HW_TIM_config_S;

/* Public Function Declarations */

// Initialize every timer listed in `config`. Validates the config (NULL
// pointers, bad numChannels, unsupported count direction, an enabled
// output-compare unit whose initial compare exceeds the period, an
// out-of-range dead-time), copies the user's htim handles into internal
// mutable storage, applies the library-managed init sequence, and starts
// each counter with its outputs disabled. Returns false on any failure.
bool HW_TIM_init(const HW_TIM_config_S * const config);

// Read the present free-running counter value of a timer in raw counts.
// Returns false if not initialized, channel out of range, or out is NULL.
bool HW_TIM_getCounter(HW_TIM_channels_E channel, uint32_t * const out);

// Set the compare value (raw counts) of an output-compare unit. Returns
// false if not initialized, indices out of range, the unit is not enabled,
// or counts exceeds the configured period.
bool HW_TIM_setCompare(HW_TIM_channels_E channel, uint8_t ocUnit, uint32_t counts);

// Read the present compare value (raw counts) of an output-compare unit.
// Same failure modes as HW_TIM_setCompare (minus the period check).
bool HW_TIM_getCompare(HW_TIM_channels_E channel, uint8_t ocUnit, uint32_t * const out);

// Enable or disable an output-compare unit's output(s). A disabled unit
// drives its configured inactive level; a complementary unit's CHxN is
// driven alongside CHx. Returns false if not initialized, indices out of
// range, or the unit is not enabled in the config.
bool HW_TIM_setOutputEnabled(HW_TIM_channels_E channel, uint8_t ocUnit, bool enabled);

#endif // HW_TIM_H
