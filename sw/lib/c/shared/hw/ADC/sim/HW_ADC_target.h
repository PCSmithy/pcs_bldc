#pragma once

// Target-specific half of HW_ADC; reached via HW_ADC.h.

/* Includes */
#include "lib_types.h"

/* Typedefs */

// One input pin on the regular conversion sequence, indexed by physical IN#.
typedef struct
{
    bool   enabled;
    char * inputNameStr;         // human-readable; aids sim trace logs
} HW_ADC_inputConfig_S;

// One input on the injected conversion sequence, indexed by sequence position.
typedef struct
{
    bool    enabled;
    char *  inputNameStr;        // human-readable; aids sim trace logs
    uint8_t pinInput;            // physical IN# this slot samples: injected and
                                 // regular share the pin (and its SIL port),
                                 // as on silicon
} HW_ADC_injectedInputConfig_S;


typedef enum
{
    HW_ADC_TIMER_TRIGGER_PWM_TIM_TRGO,
    HW_ADC_TIMER_TRIGGER_COUNT,
} HW_ADC_timerTrigger_E;

// One ADC peripheral. Lacks HAL handles; carries explicit numBits (the stm32g4
// target derives it from Init.Resolution).
typedef struct
{
    char * channelNameStr;

    HW_ADC_triggerMode_E triggerMode;
    HW_ADC_xferMode_E    xferMode;

    HW_ADC_triggerMode_E injectedTriggerMode;
    HW_ADC_xferMode_E    injectedXferMode;

    float32_t vref;
    uint8_t   numBits;           // counts -> volts conversion uses (1 << numBits) - 1

    bool configureMultimode;     // master ADC of a pair applies multimode at init

    HW_ADC_inputConfig_S         inputs[HW_ADC_INPUTS_PER_CHANNEL];
    HW_ADC_injectedInputConfig_S injectedInputs[HW_ADC_INJECTED_INPUTS_PER_CHANNEL];

    HW_ADC_timerTrigger_E injectedTimerTrigger;
    HW_ADC_triggerEdge_E  injectedTriggerEdge;
} HW_ADC_channelConfig_S;
