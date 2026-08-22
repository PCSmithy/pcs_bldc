#pragma once

/* Includes */
#include "lib_types.h"
#include "HW_ADC_channels.h"

/* Defines */

// Max input pins per ADC peripheral on the regular conversion sequence.
// STM32G4 ADC1/2 support channels 0..18 (incl. internal Vrefint, Vts,
// Vbat, and the VOPAMP outputs). Sized to 19 to index the highest
// internal channel in use (VOPAMP3 = ADC2 IN18); bump if higher-numbered
// internal channels are added.
#define HW_ADC_INPUTS_PER_CHANNEL           (19U) // TODO - move this into g4 specific file - out of generic header

// this is an MCU-architecture limit, not configurable
#define HW_ADC_INJECTED_INPUTS_PER_CHANNEL  (4U) // TODO - move this into g4 specific file - out of generic header

/* Typedefs */

// How an ADC peripheral is triggered. Software is "kick off from CPU on
// demand"; Timer is hardware-triggered (e.g. TIM1 update event for FOC
// current sampling). Used for both regular and injected paths.
typedef enum
{
    HW_ADC_TRIGGER_SOFTWARE,
    HW_ADC_TRIGGER_TIMER,
} HW_ADC_triggerMode_E;

// How conversion results are extracted: polled, ISR-driven, or DMA.
// Polled is fine for slow signals (Vbus, temp); FOC current sensing
// will eventually need ISR (injected) or DMA (regular).
typedef enum
{
    HW_ADC_XFER_POLLED,
    HW_ADC_XFER_INTERRUPT,  // currently supported only for injected channels
    HW_ADC_XFER_DMA,        // not yet implemented; init() will reject
} HW_ADC_xferMode_E;

// Per-channel outcome of the most recent _run1ms sampling pass.
// IDLE  = no pass has serviced this channel (e.g. not polled / no inputs).
// OK    = the last pass stored every enabled input's conversion.
// FAULT = a conversion timed out; some counts are stale.
typedef enum
{
    HW_ADC_CONVERSION_STATUS_IDLE,
    HW_ADC_CONVERSION_STATUS_BUSY, // a non-blocking sequence is in flight
    HW_ADC_CONVERSION_STATUS_OK,
    HW_ADC_CONVERSION_STATUS_FAULT,
} HW_ADC_conversionStatus_E;

typedef enum
{
    HW_ADC_TRIGGER_EDGE_RISING,
    HW_ADC_TRIGGER_EDGE_FALLING,
} HW_ADC_triggerEdge_E;

typedef void (*HW_ADC_injectedCallback_F)(HW_ADC_channels_E channel,
                                          HW_ADC_conversionStatus_E status,
                                          void * context);

/* Target Config */
// HW_ADC_inputConfig_S / _injectedInputConfig_S / _channelConfig_S / _timerTrigger_E
#include "HW_ADC_target.h"

typedef struct
{
    const HW_ADC_channelConfig_S * channels;
    size_t numChannels;
} HW_ADC_config_S;

/* Public Function Declarations */

bool HW_ADC_init(const HW_ADC_config_S * const config);

void HW_ADC_run1ms(void);

// inputIndex is the physical IN# (matches inputs[] indexing).
bool HW_ADC_getCount(HW_ADC_channels_E channel, uint8_t inputIndex, uint32_t * const out);

bool HW_ADC_getVolts(HW_ADC_channels_E channel, uint8_t inputIndex, float32_t * const out);

// injectedIndex is the injected sequence position 0..3 (matches
// injectedInputs[] indexing) — NOT a physical IN#. Task-context reads of
// ISR-written results are best-effort snapshots (values across channels or
// calls may span trigger events); the callback is the coherent consumer.
bool HW_ADC_getInjectedCount(HW_ADC_channels_E channel, uint8_t injectedIndex, uint32_t * const out);

bool HW_ADC_getInjectedVolts(HW_ADC_channels_E channel, uint8_t injectedIndex, float32_t * const out);

bool HW_ADC_registerInjectedCallback(HW_ADC_channels_E channel,
                                     HW_ADC_injectedCallback_F callback,
                                     void * context);

bool HW_ADC_getInjectedStatus(HW_ADC_channels_E channel,
                              HW_ADC_conversionStatus_E * const out);

// Total HAL error-callback edges on the channel since init; monotonic, no
// latched state — observe deltas to detect new errors.
bool HW_ADC_getErrorCount(HW_ADC_channels_E channel, uint32_t * const out);

bool HW_ADC_getStatus(HW_ADC_channels_E channel, HW_ADC_conversionStatus_E * const out);
