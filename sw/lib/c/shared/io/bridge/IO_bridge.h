#pragma once

/* Includes */
#include "lib_types.h"
#include "IO_bridge_channels.h"
#include "HW_TIM.h"
#include "HW_ADC.h"

/* Defines */

#define IO_BRIDGE_INJECTED_NONE  (0xFFU)

/* Typedefs */

typedef enum
{
    IO_BRIDGE_PHASE_U,
    IO_BRIDGE_PHASE_V,
    IO_BRIDGE_PHASE_W,
    IO_BRIDGE_PHASE_COUNT,
} IO_bridge_phase_E;

// voltsPerAmp of 0 marks the sense as unconfigured and makes the reader fail
// injectedIndex is the sense's position in that ADC's injected sequence
typedef struct
{
    HW_ADC_channels_E adcChannel;
    uint8_t           adcInput;
    uint8_t           injectedIndex; // Could expand to specify a derived phase channel, solve KCL programmatically, if desired,
    float32_t         zeroCurrentBias_V;    // for now, IO_bridge assumes phase W is derived
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
    uint32_t injectedPairWindow_us; // max time span between U and V phase current
                                   // sample times to consider them as synchronous
} IO_bridge_channelConfig_S;

typedef struct
{
    const IO_bridge_channelConfig_S * channels;
    size_t                            numChannels;

    // Free-running 1 us time base the injected callback stamps samples with.
    HW_TIM_peripheral_E timeBasePeripheral;
} IO_bridge_config_S;

/* Public Function Declarations */

bool IO_bridge_init(const IO_bridge_config_S * const config);

bool IO_bridge_setPhaseDuty(IO_bridge_channel_E channel, IO_bridge_phase_E phase, float32_t duty01);

bool IO_bridge_setPhaseOutputEnabled(IO_bridge_channel_E channel, IO_bridge_phase_E phase, bool enabled);

bool IO_bridge_setOutputEnabled(IO_bridge_channel_E channel, bool enabled);

bool IO_bridge_getOutputEnabled(IO_bridge_channel_E channel, bool * const enabled);

// Clear the bridge's latched hardware-break flags. Must be called once
// upon gate drive boot
bool IO_bridge_clearBreakFlags(IO_bridge_channel_E channel);

// phase current sampled in 1ms task - async to PWM period
bool IO_bridge_getPhaseCurrent(IO_bridge_channel_E channel, IO_bridge_phase_E phase, float32_t * const amps_out);

bool IO_bridge_getBusCurrent(IO_bridge_channel_E channel, float32_t * const amps_out);

// phase current sampled at center of center-aligned bright PWM
bool IO_bridge_getInjectedPhaseCurrent(IO_bridge_channel_E channel, IO_bridge_phase_E phase, float32_t * const amps_out);

bool IO_bridge_getInjectedUpdateCount(IO_bridge_channel_E channel, IO_bridge_phase_E phase, uint32_t * const out);
