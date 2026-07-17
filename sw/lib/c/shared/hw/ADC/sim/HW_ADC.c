/* Includes */

#include "lib_types.h"

#include "HW_ADC.h"
#include "HW_ADC_sim.h"
#include "SIL_ports.h"

/* Defines */

/* Typedefs */

typedef struct
{
    uint32_t counts[HW_ADC_INPUTS_PER_CHANNEL];
    uint32_t injectedCounts[HW_ADC_INJECTED_INPUTS_PER_CHANNEL];
} HW_ADC_channelData_S;

typedef struct
{
    const HW_ADC_config_S * config;
    HW_ADC_channelData_S channelData[HW_ADC_CHANNEL_COUNT];
    bool multimodeApplied[HW_ADC_CHANNEL_COUNT];

    // Per-channel outcome of the last _run1ms pass, and the SIL injection
    // hook that forces a pass to fault (HW_ADC_sim_setConversionStall).
    HW_ADC_conversionStatus_E status[HW_ADC_CHANNEL_COUNT];
    bool conversionStall[HW_ADC_CHANNEL_COUNT];

    // SIL input-port handles, one per regular input (SIL_PORTS_HANDLE_INVALID
    // when unregistered). A driven port commands the input's pin voltage.
    int32_t portHandles[HW_ADC_CHANNEL_COUNT][HW_ADC_INPUTS_PER_CHANNEL];

    uint32_t tickCounter;
    bool initialized;
} HW_ADC_data_S;

/* Private Function Declarations */

static uint32_t HW_ADC_private_voltsToCounts(double volts,
                                             const HW_ADC_channelConfig_S * const channelConfig);

/* Private Data Definitions */

static HW_ADC_data_S HW_ADC_data;
static HW_ADC_data_S * const data = &HW_ADC_data;

/* Private Function Definitions */

// Inverse of HW_ADC_getVolts: quantize a commanded pin voltage to counts the
// way the real converter would, saturating at the rails.
static uint32_t HW_ADC_private_voltsToCounts(double volts,
                                             const HW_ADC_channelConfig_S * const channelConfig)
{
    const uint32_t maxCounts = (1UL << channelConfig->numBits) - 1UL;
    uint32_t counts = 0U;
    if (volts > 0.0)
    {
        const double scaled = ((volts / (double)channelConfig->vref) * (double)maxCounts) + 0.5;
        counts = ((scaled >= (double)maxCounts)) ? maxCounts : (uint32_t)scaled;
    }
    return counts;
}

/* Public Function Definitions */

// [impl->fw~hal_adc_001~1]
// [impl->fw~hal_adc_007~1]
bool HW_ADC_init(const HW_ADC_config_S * const config)
{
    bool ret = false;
    if ((config != NULL) &&
        (config->channels != NULL) &&
        (config->numChannels <= HW_ADC_CHANNEL_COUNT))
    {
        // Validate per-channel: reject not-yet-implemented modes (both
        // regular and injected paths) and bogus numBits values that
        // would make the volts conversion misbehave. Also enforce
        // contiguous-from-zero injected enablement to match the stm32g4
        // impl's requirement.
        bool valid = true;
        for (size_t ch = 0U; ch < config->numChannels; ch++)
        {
            const HW_ADC_channelConfig_S * const channelConfig = &config->channels[ch];
            if ((channelConfig->triggerMode         != HW_ADC_TRIGGER_SOFTWARE) ||
                (channelConfig->xferMode            != HW_ADC_XFER_POLLED)      ||
                (channelConfig->injectedTriggerMode != HW_ADC_TRIGGER_SOFTWARE) ||
                (channelConfig->injectedXferMode    != HW_ADC_XFER_POLLED)      ||
                (channelConfig->numBits == 0U) ||
                (channelConfig->numBits > 31U))
            {
                valid = false;
                break;
            }

            // Injected must be contiguous from slot 0.
            bool seenDisabled = false;
            for (uint8_t i = 0U; i < HW_ADC_INJECTED_INPUTS_PER_CHANNEL; i++)
            {
                if (channelConfig->injectedInputs[i].enabled)
                {
                    if (seenDisabled)
                    {
                        valid = false;
                        break;
                    }
                }
                else
                {
                    seenDisabled = true;
                }
            }
            if (!valid)
            {
                break;
            }
        }

        if (valid)
        {
            data->config       = config;
            data->tickCounter  = 0U;
            data->initialized  = true;
            for (size_t ch = 0U; ch < HW_ADC_CHANNEL_COUNT; ch++)
            {
                for (uint8_t input = 0U; input < HW_ADC_INPUTS_PER_CHANNEL; input++)
                {
                    data->portHandles[ch][input] = SIL_PORTS_HANDLE_INVALID;
                }
            }
            for (size_t ch = 0U; ch < config->numChannels; ch++)
            {
                const HW_ADC_channelConfig_S * const channelConfig = &config->channels[ch];
                data->multimodeApplied[ch] = channelConfig->configureMultimode;

                // Register one SIL input port per enabled, named input: the
                // framework may drive its pin voltage (volts, native units);
                // undriven inputs keep the synthetic ramp. Null-safe: with no
                // hooks installed every handle stays invalid.
                for (uint8_t input = 0U; input < HW_ADC_INPUTS_PER_CHANNEL; input++)
                {
                    if ((channelConfig->inputs[input].enabled) &&
                        (channelConfig->inputs[input].inputNameStr != NULL))
                    {
                        data->portHandles[ch][input] =
                            SIL_ports_register("vsig", channelConfig->inputs[input].inputNameStr, NULL, "V");
                    }
                }
            }
            ret = true;
        }
    }
    return ret;
}

// [impl->fw~hal_adc_004~1]
// [impl->fw~hal_adc_006~1]
void HW_ADC_run1ms(void)
{
    if (data->initialized)
    {
        data->tickCounter++;

        // Synthetic ramp: each enabled input increments by 1 each tick,
        // with per-channel and per-input offsets so different inputs are
        // visually distinguishable in trace logs. Wraps at full scale.
        // Replace with sine / external injection / file replay later as
        // the SIL infrastructure grows. Same pattern for both regular
        // and injected paths; injected uses a different offset base so
        // its values don't collide with regular inputs in logs.
        for (size_t ch = 0U; ch < data->config->numChannels; ch++)
        {
            const HW_ADC_channelConfig_S * const channelConfig = &data->config->channels[ch];
            const uint32_t maxCounts = (1UL << channelConfig->numBits) - 1UL;
            const uint32_t modulo    = maxCounts + 1UL;

            // A stalled channel models a poll timeout: leave its counts
            // untouched and report the pass as a fault.
            if (data->conversionStall[ch])
            {
                data->status[ch] = HW_ADC_CONVERSION_STATUS_FAULT;
                continue;
            }

            bool sampled = false;
            for (uint8_t input = 0U; input < HW_ADC_INPUTS_PER_CHANNEL; input++)
            {
                if (channelConfig->inputs[input].enabled)
                {
                    double volts = 0.0;
                    if (SIL_ports_read(data->portHandles[ch][input], &volts))
                    {
                        // Driven SIL port: convert the commanded pin voltage
                        // to counts, as the real converter would.
                        data->channelData[ch].counts[input] =
                            HW_ADC_private_voltsToCounts(volts, channelConfig);
                    }
                    else
                    {
                        const uint32_t offset = ((uint32_t)ch * 256U) + ((uint32_t)input * 16U);
                        data->channelData[ch].counts[input] = (offset + data->tickCounter) % modulo;
                    }
                    sampled = true;
                }
            }
            for (uint8_t i = 0U; i < HW_ADC_INJECTED_INPUTS_PER_CHANNEL; i++)
            {
                if (channelConfig->injectedInputs[i].enabled)
                {
                    // Distinct offset base from regular path so trace
                    // logs can tell them apart.
                    const uint32_t offset = ((uint32_t)ch * 256U) + 0x8000U + ((uint32_t)i * 16U);
                    data->channelData[ch].injectedCounts[i] = (offset + data->tickCounter) % modulo;
                    sampled = true;
                }
            }

            data->status[ch] = (sampled) ? HW_ADC_CONVERSION_STATUS_OK : HW_ADC_CONVERSION_STATUS_IDLE;
        }
    }
}

// [impl->fw~hal_adc_002~1]
// [impl->fw~hal_adc_005~1]
bool HW_ADC_getCount(HW_ADC_channels_E channel, uint8_t inputIndex, uint32_t * const out)
{
    bool ret = false;
    if ((out != NULL) &&
        (data->initialized) &&
        (channel < HW_ADC_CHANNEL_COUNT) &&
        (inputIndex < HW_ADC_INPUTS_PER_CHANNEL) &&
        (data->config->channels[channel].inputs[inputIndex].enabled))
    {
        *out = data->channelData[channel].counts[inputIndex];
        ret = true;
    }
    return ret;
}

// [impl->fw~hal_adc_005~1]
bool HW_ADC_getVolts(HW_ADC_channels_E channel, uint8_t inputIndex, float32_t * const out)
{
    bool ret = false;
    if (out != NULL)
    {
        uint32_t counts = 0U;
        if (HW_ADC_getCount(channel, inputIndex, &counts))
        {
            const HW_ADC_channelConfig_S * const channelConfig = &data->config->channels[channel];
            const uint32_t maxCounts = (1UL << channelConfig->numBits) - 1UL;

            *out = ((float32_t)counts / (float32_t)maxCounts) * channelConfig->vref;
            ret = true;
        }
    }
    return ret;
}

// [impl->fw~hal_adc_006~1]
bool HW_ADC_getInjectedCount(HW_ADC_channels_E channel, uint8_t injectedIndex, uint32_t * const out)
{
    bool ret = false;
    if ((out != NULL) &&
        (data->initialized) &&
        (channel < HW_ADC_CHANNEL_COUNT) &&
        (injectedIndex < HW_ADC_INJECTED_INPUTS_PER_CHANNEL) &&
        (data->config->channels[channel].injectedInputs[injectedIndex].enabled))
    {
        *out = data->channelData[channel].injectedCounts[injectedIndex];
        ret = true;
    }
    return ret;
}

// [impl->fw~hal_adc_006~1]
bool HW_ADC_getInjectedVolts(HW_ADC_channels_E channel, uint8_t injectedIndex, float32_t * const out)
{
    bool ret = false;
    if (out != NULL)
    {
        uint32_t counts = 0U;
        if (HW_ADC_getInjectedCount(channel, injectedIndex, &counts))
        {
            const HW_ADC_channelConfig_S * const channelConfig = &data->config->channels[channel];
            const uint32_t maxCounts = (1UL << channelConfig->numBits) - 1UL;

            *out = ((float32_t)counts / (float32_t)maxCounts) * channelConfig->vref;
            ret = true;
        }
    }
    return ret;
}

// [impl->fw~hal_adc_004~1]
bool HW_ADC_getStatus(HW_ADC_channels_E channel, HW_ADC_conversionStatus_E * const out)
{
    bool ret = false;
    if ((out != NULL) &&
        (data->initialized) &&
        (channel < HW_ADC_CHANNEL_COUNT))
    {
        *out = data->status[channel];
        ret = true;
    }
    return ret;
}

void HW_ADC_sim_reset(void)
{
    *data = (HW_ADC_data_S){ 0 };
}

void HW_ADC_sim_setConversionStall(HW_ADC_channels_E channel, bool stall)
{
    if (channel < HW_ADC_CHANNEL_COUNT)
    {
        data->conversionStall[channel] = stall;
    }
}

// SIL inspection: was multimode applied for this channel at init?
bool HW_ADC_sim_getMultimodeApplied(HW_ADC_channels_E channel)
{
    bool applied = false;
    if (channel < HW_ADC_CHANNEL_COUNT)
    {
        applied = data->multimodeApplied[channel];
    }
    return applied;
}
