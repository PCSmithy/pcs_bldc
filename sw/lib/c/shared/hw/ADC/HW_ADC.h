#pragma once

/* Includes */
#include "lib_types.h"
#include "HW_ADC_channels.h"

/* Defines */

// Sized to index the highest internal channel in use (VOPAMP3 = ADC2 IN18);
// bump if a higher-numbered internal channel is added.
#define HW_ADC_INPUTS_PER_CHANNEL           (19U) // TODO - move this into g4 specific file - out of generic header

// this is an MCU-architecture limit, not configurable
#define HW_ADC_INJECTED_INPUTS_PER_CHANNEL  (4U) // TODO - move this into g4 specific file - out of generic header

/* Typedefs */

// Applies to both the regular and the injected path.
typedef enum
{
    HW_ADC_TRIGGER_SOFTWARE,
    HW_ADC_TRIGGER_TIMER,
} HW_ADC_triggerMode_E;

// Polled is fine for slow signals (Vbus, temp); current sensing needs the
// ISR-driven injected path.
typedef enum
{
    HW_ADC_XFER_POLLED,
    HW_ADC_XFER_INTERRUPT,  // currently supported only for injected channels
    HW_ADC_XFER_DMA,        // not yet implemented; init() will reject
} HW_ADC_xferMode_E;

// Outcome of the most recent sampling pass. IDLE = the channel was never
// serviced; FAULT = a conversion timed out, so some counts are stale.
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

// injectedIndex is the injected sequence position 0..3, NOT a physical IN#.
// Task-context reads of ISR-written results are best-effort snapshots; the
// callback is the only coherent consumer.
bool HW_ADC_getInjectedCount(HW_ADC_channels_E channel, uint8_t injectedIndex, uint32_t * const out);

bool HW_ADC_getInjectedVolts(HW_ADC_channels_E channel, uint8_t injectedIndex, float32_t * const out);

// The callback runs in the injected-completion ISR, which the board places at an
// NVIC preempt priority above configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY: it
// and everything it reaches must make no FreeRTOS call, *FromISR included.
bool HW_ADC_registerInjectedCallback(HW_ADC_channels_E channel,
                                     HW_ADC_injectedCallback_F callback,
                                     void * context);

bool HW_ADC_getInjectedStatus(HW_ADC_channels_E channel,
                              HW_ADC_conversionStatus_E * const out);

// Total HAL error-callback edges on the channel since init; monotonic, no
// latched state — observe deltas to detect new errors.
bool HW_ADC_getErrorCount(HW_ADC_channels_E channel, uint32_t * const out);

bool HW_ADC_getStatus(HW_ADC_channels_E channel, HW_ADC_conversionStatus_E * const out);
