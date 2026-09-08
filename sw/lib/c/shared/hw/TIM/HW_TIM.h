#pragma once

/* Includes */
#include "lib_types.h"
#include "HW_TIM_channels.h"

/* Defines */

// Output-compare units per timer peripheral: STM32G4 general/advanced timers
// expose CH1..CH4, and a channel's ocUnit indexes that dense space 0..3.
#define HW_TIM_OC_UNITS_PER_PERIPHERAL  (4U)

/* Typedefs */

// The function a logical channel performs on its peripheral. Only
// output-compare is implemented; HW_TIM_init rejects any future role.
typedef enum
{
    HW_TIM_ROLE_OUTPUT_COMPARE,
} HW_TIM_channelRole_E;

/* Target Config */
#include "HW_TIM_target.h"   // HW_TIM_peripheralConfig_S / HW_TIM_channelConfig_S

typedef struct
{
    const HW_TIM_peripheralConfig_S * peripherals;
    size_t numPeripherals;
    const HW_TIM_channelConfig_S * channels;
    size_t numChannels;
} HW_TIM_config_S;

/* Public Function Declarations */
//
// Every accessor below returns false when the driver is uninitialized, the
// peripheral/channel is out of range, or an out-pointer is NULL; the per-
// function notes cover only the failures beyond that.

bool HW_TIM_init(const HW_TIM_config_S * const config);

// Read the present free-running counter value of a peripheral in raw counts.
bool HW_TIM_getCounter(HW_TIM_peripheral_E peripheral, uint32_t * const out);

// Resolve the peripheral a logical channel lives on.
bool HW_TIM_getPeripheral(HW_TIM_channels_E channel, HW_TIM_peripheral_E * const out);

// Read a logical channel's peripheral counter period (ARR) in raw counts — the
// full scale a duty fraction maps against.
bool HW_TIM_getPeriod(HW_TIM_channels_E channel, uint32_t * const out);

// Set the compare value (raw counts) of a logical channel's output-compare
// unit. Also false if counts exceeds the peripheral's configured period.
bool HW_TIM_setCompare(HW_TIM_channels_E channel, uint32_t counts);

// Read the present compare value (raw counts) of a logical channel.
bool HW_TIM_getCompare(HW_TIM_channels_E channel, uint32_t * const out);

// Enable or disable a logical channel's output(s). A disabled channel drives its
// inactive level; a complementary channel's CHxN follows CHx.
bool HW_TIM_setOutputEnabled(HW_TIM_channels_E channel, bool enabled);

// Set or clear a peripheral's master output enable (MOE), the single gate over
// its enabled outputs. Commanded OFF at init; per-channel compares and enables
// are untouched, so clearing then setting restores the prior waveform.
bool HW_TIM_setMainOutputEnabled(HW_TIM_peripheral_E peripheral, bool enabled);

// Report a peripheral's master output enable. Reflects the latch's live state,
// so a break-forced clear reads back disabled though the commanded state stands.
bool HW_TIM_getMainOutputEnabled(HW_TIM_peripheral_E peripheral, bool * const enabled);

// Clear a peripheral's latched break status flags (BIF/B2IF/SBIF): a break
// latches even after the input releases. MOE is untouched.
bool HW_TIM_clearBreakFlags(HW_TIM_peripheral_E peripheral);
