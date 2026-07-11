#pragma once

/* Includes */
#include "lib_types.h"
#include "stm32g4xx_hal.h"
#include "HW_TIM_channels.h"

/* Defines */

// Output-compare units per timer peripheral. STM32G4 general/advanced timers
// expose up to CH1..CH4; a logical channel's ocUnit indexes this dense space
// 0..3 and names its TIM_CHANNEL_n in .channel.
#define HW_TIM_OC_UNITS_PER_PERIPHERAL  (4U)

// Break inputs per timer peripheral. Advanced timers (TIM1/TIM8/TIM20) have BRK
// and BRK2; breakInputs[] is indexed densely by array position, each naming its
// TIM_BREAKINPUT_* id.
#define HW_TIM_BREAK_INPUTS_PER_PERIPHERAL  (2U)

/* Typedefs */

// The function a logical channel performs on its peripheral. Only
// output-compare is implemented; the enum is the extension point for future
// roles (e.g. input capture), which HW_TIM_init rejects until built.
typedef enum
{
    HW_TIM_ROLE_OUTPUT_COMPARE,
} HW_TIM_channelRole_E;

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

typedef struct
{
    const HW_TIM_peripheralConfig_S * peripherals;
    size_t numPeripherals;
    const HW_TIM_channelConfig_S * channels;
    size_t numChannels;
} HW_TIM_config_S;

/* Public Function Declarations */

// Initialize every peripheral and logical channel in `config`. Validates the
// config (NULL pointers, bad counts, unsupported count direction, an
// out-of-range dead-time, a channel with an unsupported role or an
// out-of-range peripheral/ocUnit, an initial compare exceeding the peripheral's
// period), copies the user's htim handles into internal mutable storage,
// applies the library-managed init sequence, and starts each counter with its
// outputs disabled. Returns false on any failure.
bool HW_TIM_init(const HW_TIM_config_S * const config);

// Read the present free-running counter value of a peripheral in raw counts.
// Returns false if not initialized, peripheral out of range, or out is NULL.
bool HW_TIM_getCounter(HW_TIM_peripheral_E peripheral, uint32_t * const out);

// Resolve the peripheral a logical channel lives on. Returns false if not
// initialized, the channel is out of range, or out is NULL.
bool HW_TIM_getPeripheral(HW_TIM_channels_E channel, HW_TIM_peripheral_E * const out);

// Read a logical channel's peripheral counter period (ARR) in raw counts — the
// full-scale a duty fraction maps against. Returns false if not initialized,
// the channel is out of range, or out is NULL.
bool HW_TIM_getPeriod(HW_TIM_channels_E channel, uint32_t * const out);

// Set the compare value (raw counts) of a logical channel's output-compare
// unit. Returns false if not initialized, the channel is out of range, or
// counts exceeds the peripheral's configured period.
bool HW_TIM_setCompare(HW_TIM_channels_E channel, uint32_t counts);

// Read the present compare value (raw counts) of a logical channel. Returns
// false if not initialized, the channel is out of range, or out is NULL.
bool HW_TIM_getCompare(HW_TIM_channels_E channel, uint32_t * const out);

// Enable or disable a logical channel's output(s). A disabled channel drives
// its configured inactive level; a complementary channel's CHxN is driven
// alongside CHx. Returns false if not initialized or the channel is out of
// range.
bool HW_TIM_setOutputEnabled(HW_TIM_channels_E channel, bool enabled);

// Set or clear a peripheral's master output enable (MOE), the single gate over
// every enabled output on the peripheral. Commanded OFF at init, so outputs
// stay inactive until a consumer sets it. Per-channel compare values and
// enables are left untouched, so clearing then setting restores the prior
// waveform. Returns false if not initialized or the peripheral is out of range.
bool HW_TIM_setMainOutputEnabled(HW_TIM_peripheral_E peripheral, bool enabled);

// Report a peripheral's master output enable. Reflects hardware truth (the BDTR
// MOE bit), so a break-forced clear reads back as disabled even though the
// commanded state used by set/restore is unchanged. Returns false if not
// initialized, the peripheral is out of range, or `enabled` is NULL.
bool HW_TIM_getMainOutputEnabled(HW_TIM_peripheral_E peripheral, bool * const enabled);

// Clear a peripheral's latched break status flags (BIF/B2IF/SBIF). A break
// event latches its flag even after the input releases, so a fault present
// only at power-up would otherwise read as latched forever. MOE is untouched.
// Returns false if not initialized or the peripheral is out of range.
bool HW_TIM_clearBreakFlags(HW_TIM_peripheral_E peripheral);
