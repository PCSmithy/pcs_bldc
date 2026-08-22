#pragma once

// Target-specific half of HW_ADC; reached via HW_ADC.h.

/* Includes */
#include "lib_types.h"
#include "stm32g4xx_hal.h"

/* Typedefs */

// One input pin on the regular conversion sequence. inputs[] in the
// channel config is indexed by physical IN# (0..HW_ADC_INPUTS_PER_CHANNEL-1)
// — set .enabled=true and fill .sConfig for each input that should
// participate. .sConfig.Channel must match the slot index (e.g.
// inputs[6] must have .sConfig.Channel == ADC_CHANNEL_6); not enforced.
typedef struct
{
    bool enabled;
    ADC_ChannelConfTypeDef sConfig;
} HW_ADC_inputConfig_S;

// One input on the injected conversion sequence. injectedInputs[] is
// dense, indexed by sequence position (0..3) — slot 0 becomes injected
// rank 1, slot 1 becomes rank 2, etc. The library sets .sConfig.InjectedRank
// and .sConfig.InjectedNbrOfConversion based on array position and
// enabled count; user just fills .enabled and the rest of .sConfig.
typedef struct
{
    bool enabled;
    ADC_InjectionConfTypeDef sConfig;
} HW_ADC_injectedInputConfig_S;

typedef enum
{
    HW_ADC_TIMER_TRIGGER_TIM1_TRGO2,
    // expand with other entries from Table 167 in stm32g4 RM as needed
    HW_ADC_TIMER_TRIGGER_COUNT,
} HW_ADC_timerTrigger_E;

// One ADC peripheral.
//
// Library-managed regular-path hadc.Init fields (whatever you put here
// is silently overwritten by HW_ADC_init based on enabled-input count
// and trigger/xfer modes):
//   - NbrOfConversion       <- count of enabled regular inputs
//   - ScanConvMode          <- ENABLE iff >1 enabled regular input
//   - EOCSelection          <- ADC_EOC_SINGLE_CONV (polled needs per-conversion EOC)
//   - ContinuousConvMode    <- DISABLE (single-shot per _run1ms)
//   - LowPowerAutoWait      <- ENABLE for polled xfer (AUTDLY halts the
//                              sequencer per conversion until DR is read, so
//                              the per-rank poll+read can't overrun); left as-is otherwise
//   - ExternalTrigConv      <- ADC_SOFTWARE_START iff triggerMode == HW_ADC_TRIGGER_SOFTWARE
//
// Library-managed injected-path injectedInputs[].sConfig fields
// (similarly overwritten):
//   - InjectedRank             <- derived from array position
//   - InjectedNbrOfConversion  <- count of enabled injected inputs
//   - ExternalTrigInjecConv    <- ADC_INJECTED_SOFTWARE_START iff
//                                 injectedTriggerMode == HW_ADC_TRIGGER_SOFTWARE
//
// All other Init / sConfig fields (Resolution, DataAlign, ClockPrescaler,
// SamplingTime, etc.) are taken as-is from this config.
//
// configureMultimode applies only to the master ADC of each pair
// (e.g. ADC1 of the ADC1+2 pair); the slave's flag should be false.
typedef struct
{
    ADC_HandleTypeDef hadc;

    bool configureMultimode;
    ADC_MultiModeTypeDef multimode;

    HW_ADC_triggerMode_E triggerMode;
    HW_ADC_xferMode_E    xferMode;

    HW_ADC_triggerMode_E injectedTriggerMode;
    HW_ADC_xferMode_E    injectedXferMode;

    // Reference voltage at the analog supply rail (full-scale of the
    // ADC). Used by HW_ADC_getVolts / getInjectedVolts for counts ->
    // volts conversion. Typically 3.3f on the pcs_bldc board.
    float32_t vref;

    // Regular sequence inputs, indexed by physical IN# (sparse).
    HW_ADC_inputConfig_S inputs[HW_ADC_INPUTS_PER_CHANNEL];

    // Injected sequence inputs, indexed by sequence position (dense).
    HW_ADC_injectedInputConfig_S injectedInputs[HW_ADC_INJECTED_INPUTS_PER_CHANNEL];

    HW_ADC_timerTrigger_E injectedTimerTrigger;
    HW_ADC_triggerEdge_E  injectedTriggerEdge;

} HW_ADC_channelConfig_S;

// ADC1/2 shared-vector IRQ dispatch: stm32g4xx_it.c's ADC1_2_IRQHandler calls
// this; it services every initialized peripheral's handle (the HAL no-ops the
// one whose flags are clear).
void HW_ADC_irqHandler(void);
