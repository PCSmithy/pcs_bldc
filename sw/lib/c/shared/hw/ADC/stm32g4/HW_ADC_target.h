#pragma once

// Target-specific half of HW_ADC; reached via HW_ADC.h.

/* Includes */
#include "lib_types.h"
#include "stm32g4xx_hal.h"

/* Typedefs */

// One input pin on the regular sequence; inputs[] is indexed by physical IN#.
// .sConfig.Channel must match the slot index (inputs[6] -> ADC_CHANNEL_6);
// not enforced.
typedef struct
{
    bool enabled;
    ADC_ChannelConfTypeDef sConfig;
} HW_ADC_inputConfig_S;

// One input on the injected sequence; injectedInputs[] is dense, indexed by
// sequence position (slot 0 is injected rank 1).
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

// One ADC peripheral. HW_ADC_init silently overwrites the fields it derives
// from the enabled-input counts and the trigger/xfer modes — hadc.Init's
// NbrOfConversion, ScanConvMode, EOCSelection, ContinuousConvMode,
// LowPowerAutoWait, ExternalTrigConv, and injectedInputs[].sConfig's
// InjectedRank, InjectedNbrOfConversion, ExternalTrigInjecConv(Edge). Every
// other field is taken as-is. configureMultimode is for the master ADC of a
// pair only (ADC1 of ADC1+2); the slave's flag stays false.
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
