#pragma once

/* Includes */
#include "lib_types.h"
#include "HW_TIM_channels.h"

/* Defines */

// Output-compare units per timer peripheral. STM32G4 general/advanced timers
// expose up to CH1..CH4; a logical channel's ocUnit indexes this dense space
// 0..3.
#define HW_TIM_OC_UNITS_PER_PERIPHERAL  (4U)

/* Typedefs */

// The function a logical channel performs on its peripheral. Only
// output-compare is implemented; the enum is the extension point for future
// roles (e.g. input capture), which HW_TIM_init rejects until built.
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

// Report a peripheral's master output enable. Reflects the latch's live state,
// so a break-forced clear reads back as disabled even though the commanded
// state used by set/restore is unchanged. Returns false if not initialized, the
// peripheral is out of range, or `enabled` is NULL.
bool HW_TIM_getMainOutputEnabled(HW_TIM_peripheral_E peripheral, bool * const enabled);

// Clear a peripheral's latched break status flags (BIF/B2IF/SBIF). A break
// event latches its flag even after the input releases, so a fault present
// only at power-up would otherwise read as latched forever. MOE is untouched.
// Returns false if not initialized or the peripheral is out of range.
bool HW_TIM_clearBreakFlags(HW_TIM_peripheral_E peripheral);
