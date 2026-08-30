/* Includes */

#include "lib_types.h"

#include "HW_ADC.h"
#include "HW_TIM.h"
#include "SIL_irq.h"
#include "SIL_ports.h"

/* Defines */

// Completion dispatch rides the peripheral-ISR rung of the sim NVIC ladder
// (docs/sil/sim-interrupts.md), alongside sim HW_USB.
#define HW_ADC_IRQ_PRIORITY            (8U)

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

    // Per-channel outcome of the last _run1ms pass; a stalled channel forces
    // that pass to fault. SIL writes the stall flag by DWARF.
    HW_ADC_conversionStatus_E status[HW_ADC_CHANNEL_COUNT];
    bool conversionStall[HW_ADC_CHANNEL_COUNT];

    // SIL input-port handles, one per regular input (SIL_PORTS_HANDLE_INVALID
    // when unregistered). A driven port commands the input's pin voltage.
    int32_t portHandles[HW_ADC_CHANNEL_COUNT][HW_ADC_INPUTS_PER_CHANNEL];

    // Timer-triggered injected engine. Triggers land during the TIM advance
    // (platform-tick context); values are sampled there, and the pended
    // completion interrupt drains them in the firmware fiber's ISR bracket,
    // as on hardware.
    HW_ADC_conversionStatus_E injectedStatus[HW_ADC_CHANNEL_COUNT];
    HW_ADC_injectedCallback_F injectedCallback[HW_ADC_CHANNEL_COUNT];
    void *                    injectedCallbackContext[HW_ADC_CHANNEL_COUNT];
    uint32_t                  pendingCompletions[HW_ADC_CHANNEL_COUNT];

    uint32_t tickCounter;
    bool initialized;
} HW_ADC_data_S;

/* Private Data Declarations */

// Which sim timer peripheral each trigger source names.
static const HW_TIM_peripheral_E HW_ADC_triggerPeripheralMapping[HW_ADC_TIMER_TRIGGER_COUNT] =
{
    [HW_ADC_TIMER_TRIGGER_PWM_TIM_TRGO] = HW_TIM_PERIPHERAL_1,
};

/* Private Function Declarations */

static uint32_t HW_ADC_private_voltsToCounts(double volts,
                                             const HW_ADC_channelConfig_S * const channelConfig);
static bool HW_ADC_private_isInjectedTriggered(const HW_ADC_channelConfig_S * const channelConfig);
static void HW_ADC_private_trgoHandler(HW_TIM_peripheral_E peripheral,
                                       HW_TIM_trgoCross_E cross, void * context);
// External linkage (the HW_USB_sim_irqHandler pattern): SIL scenarios resolve
// the completion ISR by name, which -O2 strips from a static.
void HW_ADC_sim_completionDispatch(void);

/* Private Data Definitions */

static HW_ADC_data_S HW_ADC_data;
static HW_ADC_data_S * const data = &HW_ADC_data;

// The completion service's framework handle. Lives outside HW_ADC_data so the
// re-entrant init's clean slate can still cancel the previous registration.
static int32_t HW_ADC_completionIrqHandle = SIL_IRQ_HANDLE_INVALID;

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

static bool HW_ADC_private_isInjectedTriggered(const HW_ADC_channelConfig_S * const channelConfig)
{
    return ((channelConfig->injectedTriggerMode == HW_ADC_TRIGGER_TIMER) &&
            (channelConfig->injectedXferMode    == HW_ADC_XFER_INTERRUPT));
}

// [impl->fw~hal_adc_003~1]
// TRGO sink: fires once per trigger event during HW_TIM_advanceTime. Samples
// every triggered channel wired to this peripheral whose edge select accepts
// the crossing, then defers completion to the one-shot below.
static void HW_ADC_private_trgoHandler(HW_TIM_peripheral_E peripheral,
                                       HW_TIM_trgoCross_E cross, void * context)
{
    (void)context;
    if (data->initialized)
    {
        for (size_t ch = 0U; ch < data->config->numChannels; ch++)
        {
            const HW_ADC_channelConfig_S * const channelConfig = &data->config->channels[ch];
            if ((!HW_ADC_private_isInjectedTriggered(channelConfig)) ||
                (HW_ADC_triggerPeripheralMapping[channelConfig->injectedTimerTrigger] != peripheral))
            {
                continue;
            }

            // Edge select, the JEXTEN twin: the board's trigger point is OC4
            // in PWM2, whose OCREF rising edge is the up-count crossing.
            const HW_TIM_trgoCross_E accepted =
                (channelConfig->injectedTriggerEdge == HW_ADC_TRIGGER_EDGE_RISING)
                    ? HW_TIM_TRGO_CROSS_UP
                    : HW_TIM_TRGO_CROSS_DOWN;
            if (cross != accepted)
            {
                continue;
            }

            // Sample at the trigger instant: each slot reads its pin's port
            // (shared with the regular path, as on silicon); undriven pins
            // keep the synthetic ramp.
            bool sampled = false;
            for (uint8_t i = 0U; i < HW_ADC_INJECTED_INPUTS_PER_CHANNEL; i++)
            {
                if (channelConfig->injectedInputs[i].enabled)
                {
                    const uint8_t pin = channelConfig->injectedInputs[i].pinInput;
                    double volts = 0.0;
                    if (SIL_ports_read(data->portHandles[ch][pin], &volts))
                    {
                        data->channelData[ch].injectedCounts[i] =
                            HW_ADC_private_voltsToCounts(volts, channelConfig);
                    }
                    else
                    {
                        const uint32_t modulo = (1UL << channelConfig->numBits);
                        const uint32_t offset = ((uint32_t)ch * 256U) + 0x8000U + ((uint32_t)i * 16U);
                        data->channelData[ch].injectedCounts[i] = (offset + data->tickCounter) % modulo;
                    }
                    sampled = true;
                }
            }
            if (sampled)
            {
                data->pendingCompletions[ch]++;
                // The NVIC twin of JEOS raising ADC1_2: queue the result,
                // pend the completion interrupt.
                SIL_irq_pend(HW_ADC_completionIrqHandle);
            }
        }
    }
}

// [impl->fw~hal_adc_008~1]
// Completion interrupt: pended by the trigger handler, dispatched in the
// firmware fiber during the same step's ISR phase. Drains every pending
// conversion — one status write + callback per trigger event, so the
// completion count matches the trigger cadence on any grid.
void HW_ADC_sim_completionDispatch(void)
{
    if (data->initialized)
    {
        for (size_t ch = 0U; ch < HW_ADC_CHANNEL_COUNT; ch++)
        {
            while (data->pendingCompletions[ch] > 0U)
            {
                data->pendingCompletions[ch]--;
                data->injectedStatus[ch] = HW_ADC_CONVERSION_STATUS_OK;
                if (data->injectedCallback[ch] != NULL)
                {
                    data->injectedCallback[ch]((HW_ADC_channels_E)ch,
                                               data->injectedStatus[ch],
                                               data->injectedCallbackContext[ch]);
                }
            }
        }
    }
}

/* Public Function Definitions */

// [impl->fw~hal_adc_001~1]
// [impl->fw~hal_adc_007~1]
bool HW_ADC_init(const HW_ADC_config_S * const config)
{
    bool ret = false;

    // Re-entrant: every call, accepted or rejected, drops the driver back to
    // its uninitialized state, so a second init in one process is a clean slate.
    // The previous completion service goes with it.
    SIL_irq_cancel(HW_ADC_completionIrqHandle);
    HW_ADC_completionIrqHandle = SIL_IRQ_HANDLE_INVALID;
    *data = (HW_ADC_data_S){ 0 };
    for (size_t ch = 0U; ch < HW_ADC_CHANNEL_COUNT; ch++)
    {
        for (uint8_t input = 0U; input < HW_ADC_INPUTS_PER_CHANNEL; input++)
        {
            data->portHandles[ch][input] = SIL_PORTS_HANDLE_INVALID;
        }
    }

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
            const bool injectedPolled =
                ((channelConfig->injectedTriggerMode == HW_ADC_TRIGGER_SOFTWARE) &&
                 (channelConfig->injectedXferMode    == HW_ADC_XFER_POLLED));
            const bool injectedTriggered =
                (HW_ADC_private_isInjectedTriggered(channelConfig) &&
                 (channelConfig->injectedTimerTrigger < HW_ADC_TIMER_TRIGGER_COUNT));
            if ((channelConfig->triggerMode != HW_ADC_TRIGGER_SOFTWARE) ||
                (channelConfig->xferMode    != HW_ADC_XFER_POLLED)      ||
                ((!injectedPolled) && (!injectedTriggered))             ||
                (channelConfig->numBits == 0U) ||
                (channelConfig->numBits > 31U))
            {
                valid = false;
                break;
            }

            // A triggered slot samples a pin's port; the pin index must be real.
            if (injectedTriggered)
            {
                for (uint8_t i = 0U; i < HW_ADC_INJECTED_INPUTS_PER_CHANNEL; i++)
                {
                    if ((channelConfig->injectedInputs[i].enabled) &&
                        (channelConfig->injectedInputs[i].pinInput >= HW_ADC_INPUTS_PER_CHANNEL))
                    {
                        valid = false;
                        break;
                    }
                }
                if (!valid)
                {
                    break;
                }
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
                            SIL_ports_register("vsig", channelConfig->inputs[input].inputNameStr, "V");
                    }
                }

                // [impl->fw~hal_adc_003~1]
                // Arm the timer-triggered injected path: BUSY until the first
                // completion, and one TRGO sink serves every triggered channel
                // (the modeled trigger line fans out, as TRGO2 does).
                if (HW_ADC_private_isInjectedTriggered(channelConfig))
                {
                    bool anyEnabled = false;
                    for (uint8_t i = 0U; i < HW_ADC_INJECTED_INPUTS_PER_CHANNEL; i++)
                    {
                        anyEnabled = anyEnabled || channelConfig->injectedInputs[i].enabled;
                    }
                    if (anyEnabled)
                    {
                        data->injectedStatus[ch] = HW_ADC_CONVERSION_STATUS_BUSY;
                        (void)HW_TIM_registerTrgoCallback(
                            HW_ADC_triggerPeripheralMapping[channelConfig->injectedTimerTrigger],
                            HW_ADC_private_trgoHandler, NULL);
                        if (HW_ADC_completionIrqHandle == SIL_IRQ_HANDLE_INVALID)
                        {
                            HW_ADC_completionIrqHandle = SIL_irq_registerPended(
                                HW_ADC_sim_completionDispatch,
                                HW_ADC_IRQ_PRIORITY);
                        }
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
            // Timer-triggered injected slots are owned by the TRGO path; only
            // the polled flavor rides this pass.
            if (!HW_ADC_private_isInjectedTriggered(channelConfig))
            {
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

// [impl->fw~hal_adc_008~1]
bool HW_ADC_registerInjectedCallback(HW_ADC_channels_E channel,
                                     HW_ADC_injectedCallback_F callback,
                                     void * context)
{
    bool ret = false;
    if ((data->initialized) &&
        (channel < HW_ADC_CHANNEL_COUNT))
    {
        // may be NULL
        data->injectedCallback[channel]        = callback;
        data->injectedCallbackContext[channel] = context;
        ret = true;
    }
    return ret;
}

// [impl->fw~hal_adc_008~1]
bool HW_ADC_getInjectedStatus(HW_ADC_channels_E channel,
                              HW_ADC_conversionStatus_E * const out)
{
    bool ret = false;
    if ((data->initialized) &&
        (channel < HW_ADC_CHANNEL_COUNT) &&
        (out != NULL))
    {
        *out = data->injectedStatus[channel];
        ret = true;
    }
    return ret;
}
