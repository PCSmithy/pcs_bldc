#pragma once

/* Includes */
#include "lib_types.h"
#include "IO_PWM_channels.h"
#include "HW_TIM.h"

/* Typedefs */

// One motor phase: the HW_TIM channel and output-compare unit that carry its
// complementary PWM. The unit is configured complementary in the HW_TIM
// channel config, so driving one duty command actuates the phase's high- and
// low-side gate signals in antiphase with dead-time.
typedef struct
{
    HW_TIM_channels_E timChannel;   // HW_TIM channel carrying this phase's OC unit
    uint8_t           ocUnit;       // output-compare unit index on that channel
} IO_PWM_phaseConfig_S;

typedef struct
{
    const IO_PWM_phaseConfig_S * phases;
    size_t                       numPhases;
    // The timer whose master output enable gates the whole bridge. All phases
    // share it, so a single set/clear energizes or dark-holds every gate line.
    HW_TIM_channels_E            bridgeTimChannel;
} IO_PWM_config_S;

/* Public Function Declarations */

// Validate the config and initialize every configured phase, enabling each
// phase's output-compare unit so the bridge master output enable is the single
// runtime gate over the outputs. The bridge starts disabled (HW_TIM commands
// MOE off at init), so no phase drives until IO_PWM_setOutputEnabled(true).
// Returns false on a NULL config/phase array, a phase count over the available
// phases, or a phase whose HW_TIM channel or output-compare unit is out of
// range.
bool IO_PWM_init(const IO_PWM_config_S * const config);

// Apply a per-phase duty in [0, 1] as the fraction of the PWM period the
// phase's primary output is active. The compare value is round(duty x period),
// so duty 1 maps to the full timer period. Returns false for a duty outside
// [0, 1], an out-of-range phase, or an uninitialized driver.
bool IO_PWM_setDuty(IO_PWM_phase_E phase, float32_t duty);

// Set the bridge output enable through the phase timer's master output enable.
// While disabled every phase output holds its inactive level; duty commands
// issued meanwhile still land on the compare registers and take effect at
// re-enable. Returns false if the driver is uninitialized.
bool IO_PWM_setOutputEnabled(bool enabled);

// Report the bridge output-enable state from the phase timer's master output
// enable. Reflects hardware truth, so a break-forced disable reads back as
// disabled. Returns false if uninitialized or `enabled` is NULL.
bool IO_PWM_getOutputEnabled(bool * const enabled);
