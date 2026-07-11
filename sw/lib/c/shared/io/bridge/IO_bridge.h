#pragma once

/* Includes */
#include "lib_types.h"
#include "IO_bridge_channels.h"
#include "HW_TIM.h"
#include "HW_ADC.h"

/* Typedefs */

typedef enum
{
    IO_BRIDGE_PHASE_U,
    IO_BRIDGE_PHASE_V,
    IO_BRIDGE_PHASE_W,
    IO_BRIDGE_PHASE_COUNT,
} IO_bridge_phase_E;

// One current-sense front end: the ADC (channel, physical IN#) the shunt
// amplifier feeds, and the linear scaling that turns its volts into amps —
// i = (v - zeroCurrentBias_V) / voltsPerAmp. A voltsPerAmp of 0 marks the
// sense as unconfigured and makes the reader fail rather than divide by zero.
typedef struct
{
    HW_ADC_channels_E adcChannel;
    uint8_t           adcInput;
    float32_t         zeroCurrentBias_V;
    float32_t         voltsPerAmp;
} IO_bridge_currentSenseConfig_S;

typedef struct
{
    HW_TIM_channels_E phaseU;
    HW_TIM_channels_E phaseV;
    HW_TIM_channels_E phaseW;

    // Phase-shunt sense (indexed by IO_bridge_phase_E) and the DC-bus input
    // sense. Read back in engineering units via the getCurrent accessors.
    IO_bridge_currentSenseConfig_S phaseCurrent[IO_BRIDGE_PHASE_COUNT];
    IO_bridge_currentSenseConfig_S busCurrent;
} IO_bridge_channelConfig_S;

typedef struct
{
    const IO_bridge_channelConfig_S * channels;
    size_t                            numChannels;
} IO_bridge_config_S;

/* Public Function Declarations */

bool IO_bridge_init(const IO_bridge_config_S * const config);

// Returns false for a duty outside [0, 1], an out-of-range bridge or phase,
// or an uninitialized driver.
bool IO_bridge_setPhaseDuty(IO_bridge_channel_E channel, IO_bridge_phase_E phase, float32_t duty01);

// Enable or disable one phase's output. A disabled phase holds its outputs at
// the inactive level (both gate lines off) while the other phases keep driving.
// Returns false for an out-of-range bridge or phase, or an uninitialized driver.
bool IO_bridge_setPhaseOutputEnabled(IO_bridge_channel_E channel, IO_bridge_phase_E phase, bool enabled);

// Set the bridge output enable through the phases' shared master output enable.
// While disabled every phase output holds its inactive level; duty commands
// issued meanwhile still land on the compare registers and take effect at
// re-enable. Returns false for an out-of-range bridge or an uninitialized
// driver.
bool IO_bridge_setOutputEnabled(IO_bridge_channel_E channel, bool enabled);

// Report the bridge output-enable state from the shared master output enable.
// Reflects hardware truth, so a break-forced disable reads back as disabled.
// Returns false for an out-of-range bridge, an uninitialized driver, or a NULL
// `enabled`.
bool IO_bridge_getOutputEnabled(IO_bridge_channel_E channel, bool * const enabled);

// Clear the bridge's latched hardware-break flags. Intended for the one stale
// latch every boot leaves behind (the gate driver holds its fault line active
// until configured); afterwards a latched flag is real fault history. Returns
// false for an out-of-range bridge or an uninitialized driver.
bool IO_bridge_clearBreakFlags(IO_bridge_channel_E channel);

// Read one phase's sensed current in amps (signed; sign follows the shunt
// front end's zero-current bias). Returns false — leaving *amps_out unchanged —
// for an out-of-range bridge or phase, an uninitialized driver, a NULL
// destination, an unconfigured sense (voltsPerAmp == 0), or an ADC read
// failure.
bool IO_bridge_getPhaseCurrent(IO_bridge_channel_E channel, IO_bridge_phase_E phase, float32_t * const amps_out);

// Read the bridge's DC-bus input current in amps. Same failure modes as
// IO_bridge_getPhaseCurrent (minus the phase argument).
bool IO_bridge_getBusCurrent(IO_bridge_channel_E channel, float32_t * const amps_out);
