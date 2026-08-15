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
#define HW_ADC_INPUTS_PER_CHANNEL           (19U)

// Max inputs per ADC peripheral on the injected sequence. STM32G4 ADCs
// have exactly 4 injected slots (JDR1..JDR4); this is an MCU-architecture
// limit, not a configurable.
#define HW_ADC_INJECTED_INPUTS_PER_CHANNEL  (4U)

/* Typedefs */

// How an ADC peripheral is triggered. Software is "kick off from CPU on
// demand"; Timer is hardware-triggered (e.g. TIM1 update event for FOC
// current sampling). Used for both regular and injected paths.
typedef enum
{
    HW_ADC_TRIGGER_SOFTWARE,
    HW_ADC_TRIGGER_TIMER,        // not yet implemented; init() will reject
} HW_ADC_triggerMode_E;

// How conversion results are extracted: polled, ISR-driven, or DMA.
// Polled is fine for slow signals (Vbus, temp); FOC current sensing
// will eventually need ISR (injected) or DMA (regular).
typedef enum
{
    HW_ADC_XFER_POLLED,
    HW_ADC_XFER_INTERRUPT,       // not yet implemented; init() will reject
    HW_ADC_XFER_DMA,             // not yet implemented; init() will reject
} HW_ADC_xferMode_E;

// Per-channel outcome of the most recent _run1ms sampling pass.
// IDLE  = no pass has serviced this channel (e.g. not polled / no inputs).
// OK    = the last pass stored every enabled input's conversion.
// FAULT = a conversion timed out; some counts are stale.
typedef enum
{
    HW_ADC_CONVERSION_STATUS_IDLE,
    HW_ADC_CONVERSION_STATUS_OK,
    HW_ADC_CONVERSION_STATUS_FAULT,
} HW_ADC_conversionStatus_E;

/* Target Config */
#include "HW_ADC_target.h"   // HW_ADC_inputConfig_S / _injectedInputConfig_S / _channelConfig_S

typedef struct
{
    const HW_ADC_channelConfig_S * channels;
    size_t numChannels;
} HW_ADC_config_S;

/* Public Function Declarations */

// Initialize all ADC peripherals listed in `config`. Validates the
// config (NULL ptrs, bad numChannels, unsupported trigger/xfer modes,
// out-of-range Rank values), applies the library-managed per-target
// setup, and configures each enabled regular and injected input.
// Returns false on any failure.
bool HW_ADC_init(const HW_ADC_config_S * const config);

// Sample all enabled inputs on every channel using SOFTWARE+POLLED.
// Software-triggers each peripheral's regular sequence, polls per-
// conversion EOC, and stores results indexed by physical IN#. Then
// software-triggers each peripheral's injected sequence (if any),
// polls for sequence end, and stores results indexed by injected
// sequence position. Channels with other trigger/xfer modes are
// skipped (their data sources will populate the count buffers
// asynchronously). Safe to call before init (no-op).
void HW_ADC_run1ms(void);

// Read the most recent raw count from the regular conversion sequence
// for a given (channel, IN#). Returns false if not initialized,
// indices out of range, input not enabled, or out is NULL.
bool HW_ADC_getCount(HW_ADC_channels_E channel, uint8_t inputIndex, uint32_t * const out);

// Read the most recent regular reading converted to volts (counts *
// vref / (2^numBits - 1)). Same failure modes as HW_ADC_getCount.
bool HW_ADC_getVolts(HW_ADC_channels_E channel, uint8_t inputIndex, float32_t * const out);

// Read the most recent raw count from the injected conversion sequence
// for a given (channel, sequence position). injectedIndex is 0..3 (NOT
// physical IN# — injected uses dense sequence-position indexing).
// Returns false if not initialized, indices out of range, slot not
// enabled, or out is NULL.
bool HW_ADC_getInjectedCount(HW_ADC_channels_E channel, uint8_t injectedIndex, uint32_t * const out);

// Volts version of HW_ADC_getInjectedCount. Uses the same vref + numBits
// conversion as HW_ADC_getVolts.
bool HW_ADC_getInjectedVolts(HW_ADC_channels_E channel, uint8_t injectedIndex, float32_t * const out);

// Read the conversion status of a channel's most recent _run1ms pass.
// Returns false if not initialized, channel out of range, or out is NULL.
bool HW_ADC_getStatus(HW_ADC_channels_E channel, HW_ADC_conversionStatus_E * const out);
