#ifndef HW_ADC_H
#define HW_ADC_H

/* Includes */
#include "lib_types.h"
#include "HW_ADC_channels.h"

/* Defines */

#define HW_ADC_INPUTS_PER_CHANNEL           (16U)
#define HW_ADC_INJECTED_INPUTS_PER_CHANNEL  (4U)

/* Typedefs */

// Mirror of the stm32g4 enums. Same names + numeric values so project
// channel-config files reference the same identifiers regardless of
// target. Sim has no real trigger/xfer hardware, but the enums shape
// the API uniformly across both impls.
typedef enum
{
    HW_ADC_TRIGGER_SOFTWARE,
    HW_ADC_TRIGGER_TIMER,        // not yet implemented; init() will reject
} HW_ADC_triggerMode_E;

typedef enum
{
    HW_ADC_XFER_POLLED,
    HW_ADC_XFER_INTERRUPT,       // not yet implemented; init() will reject
    HW_ADC_XFER_DMA,             // not yet implemented; init() will reject
} HW_ADC_xferMode_E;

// Per-channel outcome of the most recent _run1ms sampling pass. Mirror of
// the stm32g4 enum (same names + values). On the sim, FAULT is produced by
// HW_ADC_sim_setConversionStall (no real conversion to time out).
typedef enum
{
    HW_ADC_CONVERSION_STATUS_IDLE,
    HW_ADC_CONVERSION_STATUS_OK,
    HW_ADC_CONVERSION_STATUS_FAULT,
} HW_ADC_conversionStatus_E;

typedef struct
{
    bool   enabled;
    char * inputNameStr;         // human-readable; aids sim trace logs
} HW_ADC_inputConfig_S;

typedef struct
{
    bool   enabled;
    char * inputNameStr;         // human-readable; aids sim trace logs
} HW_ADC_injectedInputConfig_S;

// Mirror of the stm32g4 struct. Lacks HAL handles; gains explicit
// numBits (stm32g4 derives it from Init.Resolution, sim has no Init).
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
} HW_ADC_channelConfig_S;

typedef struct
{
    const HW_ADC_channelConfig_S * channels;
    size_t numChannels;
} HW_ADC_config_S;

/* Public Function Declarations */

bool HW_ADC_init(const HW_ADC_config_S * const config);
void HW_ADC_run1ms(void);

bool HW_ADC_getCount(HW_ADC_channels_E channel, uint8_t inputIndex, uint32_t * const out);
bool HW_ADC_getVolts(HW_ADC_channels_E channel, uint8_t inputIndex, float32_t * const out);

bool HW_ADC_getInjectedCount(HW_ADC_channels_E channel, uint8_t injectedIndex, uint32_t * const out);
bool HW_ADC_getInjectedVolts(HW_ADC_channels_E channel, uint8_t injectedIndex, float32_t * const out);

bool HW_ADC_getStatus(HW_ADC_channels_E channel, HW_ADC_conversionStatus_E * const out);

#endif // HW_ADC_H
