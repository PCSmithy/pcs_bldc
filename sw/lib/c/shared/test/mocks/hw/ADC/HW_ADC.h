#pragma once

// Minimal mock of the HW_ADC public header — the surface consumers of the ADC
// use: the regular sequence (getVolts) and the timer-triggered injected
// sequence (getInjectedVolts plus completion-callback registration). The
// implementation lives in mock_HW_ADC.c, driven by the controls in
// mock_HW_ADC.h.

#include "lib_types.h"
#include "HW_ADC_channels.h"

/* Defines */

#define HW_ADC_INJECTED_INPUTS_PER_CHANNEL  (4U)

/* Typedefs */

typedef enum
{
    HW_ADC_CONVERSION_STATUS_IDLE,
    HW_ADC_CONVERSION_STATUS_BUSY,
    HW_ADC_CONVERSION_STATUS_OK,
    HW_ADC_CONVERSION_STATUS_FAULT,
} HW_ADC_conversionStatus_E;

typedef void (*HW_ADC_injectedCallback_F)(HW_ADC_channels_E channel,
                                          HW_ADC_conversionStatus_E status,
                                          void * context);

/* Public Function Declarations */

bool HW_ADC_getVolts(HW_ADC_channels_E channel, uint8_t inputIndex, float32_t * const out);

bool HW_ADC_getInjectedVolts(HW_ADC_channels_E channel, uint8_t injectedIndex, float32_t * const out);

bool HW_ADC_registerInjectedCallback(HW_ADC_channels_E channel,
                                     HW_ADC_injectedCallback_F callback,
                                     void * context);
