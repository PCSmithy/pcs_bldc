/* Includes */

#include "lib_types.h"

#include "HW_ADC.h"
#include "HW_DMA.h"
#include "HW_TIM.h"
#include "SIL_irq.h"
#include "SIL_ports.h"

/* Defines */

// Completion dispatch rides the peripheral-ISR rung of the sim NVIC ladder
// (docs/sil/sim-interrupts.md), alongside sim HW_USB.
#define HW_ADC_IRQ_PRIORITY            (8U)

// Synthetic-ramp offset base for injected slots, so trace logs can tell them
// apart from regular inputs.
#define HW_ADC_INJECTED_RAMP_BASE      (0x8000U)

/* Typedefs */

// Mirrors the stm32g4 driver's per-channel state; the sim-only tail stands in
// for the HAL handle with the SIL seams.
typedef struct
{
    // Per-channel derived state.
    uint8_t numEnabledInputs;
    uint8_t numEnabledInjectedInputs;

    // rankOrder[r] = physical IN# that occupies regular-sequence rank (r+1);
    // ascending IN# in the sim.
    uint8_t rankOrder[HW_ADC_INPUTS_PER_CHANNEL];

    // Resolution (bits) for the volts conversion.
    uint8_t numBits;

    // Regular-sequence results, indexed by physical IN# (sparse storage,
    // matches user's inputs[] indexing). Undefined until the channel's first
    // completed pass sets countsValid; reads refuse them before that.
    uint32_t counts[HW_ADC_INPUTS_PER_CHANNEL];
    bool countsValid;

    // indexed in Rank order
    uint16_t dmaDestinationBuffer[HW_ADC_INPUTS_PER_CHANNEL];

    // Injected-sequence results, indexed by sequence position (dense,
    // matches user's injectedInputs[] indexing).
    uint32_t injectedCounts[HW_ADC_INJECTED_INPUTS_PER_CHANNEL];

    // Outcome of the most recent _run1ms pass on this channel.
    HW_ADC_conversionStatus_E status;
    HW_ADC_callback_F conversionCompleteCallback;
    void * conversionCompleteCallbackContext;

    HW_ADC_conversionStatus_E injectedConversionStatus;
    HW_ADC_callback_F injectedConversionCompleteCallback;
    void * injectedConversionCompleteCallbackContext;

    // Injected error edges. No sim fault source writes it yet; SIL can drive it
    // by DWARF the way the stall knobs are driven.
    uint32_t errorCount;

    // flag to discard any output from a latent callback after a hung DMA transfer
    bool discardTransfer;

    /* sim-only */

    // The sim's data-register stream: rank-ordered samples taken at the pass
    // start, landed in dmaDestinationBuffer at the transfer's completion.
    uint16_t dmaSourceBuffer[HW_ADC_INPUTS_PER_CHANNEL];

    // SIL input-port handles, one per regular input (SIL_PORTS_HANDLE_INVALID
    // when unregistered). A driven port commands the input's pin voltage.
    int32_t portHandles[HW_ADC_INPUTS_PER_CHANNEL];

    // Polled-path stall knob, written by DWARF from SIL: the pass faults and
    // leaves the counts untouched. A DMA pass stalls in HW_DMA instead.
    bool conversionStall;

    // Injected triggers sampled but not yet drained by the completion interrupt.
    uint32_t pendingCompletions;

    bool multimodeApplied;
} HW_ADC_channelData_S;

typedef struct
{
    const HW_ADC_config_S * config;

    HW_ADC_channelData_S channelData[HW_ADC_CHANNEL_COUNT];

    uint32_t tickCounter;   // sim-only: drives the synthetic ramps

    bool initialized;
} HW_ADC_data_S;

/* Private Function Declarations */

static uint32_t HW_ADC_private_voltsToCounts(double volts, const HW_ADC_channelConfig_S * const channelConfig);
static uint32_t HW_ADC_private_sampleInput(HW_ADC_channels_E channel, uint8_t input, uint32_t rampOffset);
static bool     HW_ADC_private_initOneChannel(HW_ADC_channels_E channel);
static bool     HW_ADC_private_armTriggeredChannels(void);
static void     HW_ADC_private_dmaXferComplete(HW_DMA_channel_E dmaChannel, void * context);
static void     HW_ADC_private_trgoHandler(HW_TIM_peripheral_E peripheral, HW_TIM_trgoCross_E cross, void * context);
static void     HW_ADC_private_startPolledRegularConversion(HW_ADC_channels_E channel);
static void     HW_ADC_private_startDmaRegularConversion(HW_ADC_channels_E channel);
// External linkage (the HW_USB_sim_irqHandler pattern): SIL scenarios resolve
// the completion ISR by name, which -O2 strips from a static.
void HW_ADC_sim_completionDispatch(void);

/* Private Data Definitions */

static HW_ADC_data_S HW_ADC_data;
static HW_ADC_data_S * const data = &HW_ADC_data;

// Which sim timer peripheral each trigger source names.
static const HW_TIM_peripheral_E HW_ADC_triggerPeripheralMapping[HW_ADC_INJECTED_TRIGGER_COUNT] =
{
    [HW_ADC_INJECTED_TRIGGER_PWM_TIM_TRGO] = HW_TIM_PERIPHERAL_1,
};

// The injected completion service's framework handle. Lives outside
// HW_ADC_data so the re-entrant init's clean slate can still cancel the
// previous registration.
static int32_t HW_ADC_completionIrqHandle = SIL_IRQ_HANDLE_INVALID;

/* Private Function Definitions */

// Inverse of HW_ADC_getVolts: quantize a commanded pin voltage to counts the
// way the real converter would, saturating at the rails.
static uint32_t HW_ADC_private_voltsToCounts(double volts, const HW_ADC_channelConfig_S * const channelConfig)
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

// One input's sample: a driven SIL port's commanded voltage as counts, or the
// synthetic ramp (rampOffset + tick, wrapping at full scale) when undriven.
static uint32_t HW_ADC_private_sampleInput(HW_ADC_channels_E channel, uint8_t input, uint32_t rampOffset)
{
    const HW_ADC_channelConfig_S * const channelConfig = &data->config->channels[channel];
    uint32_t counts = 0U;
    double volts = 0.0;
    if (SIL_ports_read(data->channelData[channel].portHandles[input], &volts))
    {
        counts = HW_ADC_private_voltsToCounts(volts, channelConfig);
    }
    else
    {
        const uint32_t modulo = (1UL << channelConfig->numBits);
        counts = (rampOffset + data->tickCounter) % modulo;
    }
    return counts;
}

static bool HW_ADC_private_initOneChannel(HW_ADC_channels_E channel)
{
    bool ret = true;
    const HW_ADC_channelConfig_S * const channelConfig = &data->config->channels[channel];
    HW_ADC_channelData_S * const channelData = &data->channelData[channel];

    // Feature guard: reject trigger/transfer combinations the driver has not
    // built, rather than silently misconfiguring
    const bool regularPolled    = ((channelConfig->triggerMode == HW_ADC_TRIGGER_SOFTWARE) &&
                                   (channelConfig->xferMode == HW_ADC_XFER_POLLED));
    const bool regularDMA       = ((channelConfig->triggerMode == HW_ADC_TRIGGER_SOFTWARE) &&
                                   (channelConfig->xferMode == HW_ADC_XFER_DMA));
    if ((!regularPolled) && (!regularDMA))
    {
        ret = false;
    }

    // A bogus resolution would make the volts conversion misbehave.
    if ((channelConfig->numBits == 0U) || (channelConfig->numBits > 31U))
    {
        ret = false;
    }

    // [impl->fw~hal_adc_002~1]
    // Walk the sparse regular inputs[] array and build the rank-ordered IN#
    // list: the sim converts enabled inputs in ascending IN#, so the ranks are
    // contiguous by construction.
    uint8_t numEnabledRegular = 0U;
    uint8_t rankOrder[HW_ADC_INPUTS_PER_CHANNEL] = { 0 };
    if (ret)
    {
        for (uint8_t input = 0U; input < HW_ADC_INPUTS_PER_CHANNEL; input++)
        {
            if (channelConfig->inputs[input].enabled)
            {
                rankOrder[numEnabledRegular] = input;
                numEnabledRegular++;
            }
        }
    }

    // Count enabled injected inputs. Storage is dense: slot N is rank N+1.
    // Enforce contiguous-from-zero.
    uint8_t numEnabledInjected = 0U;
    if (ret)
    {
        bool seenDisabled = false;
        for (uint8_t i = 0U; i < HW_ADC_INJECTED_INPUTS_PER_CHANNEL; i++)
        {
            if (channelConfig->injectedInputs[i].enabled)
            {
                if (seenDisabled)
                {
                    ret = false;
                    break;
                }
                numEnabledInjected++;
            }
            else
            {
                seenDisabled = true;
            }
        }
    }

    // The injected sequence is hardware-triggered with interrupt completion;
    // an enabled sequence needs a known trigger source, and each slot samples
    // a pin's port, so the pin index must be real.
    if (ret && (numEnabledInjected > 0U))
    {
        if (channelConfig->injectedTrigger >= HW_ADC_INJECTED_TRIGGER_COUNT)
        {
            ret = false;
        }
        for (uint8_t i = 0U; i < numEnabledInjected; i++)
        {
            if (channelConfig->injectedInputs[i].pinInput >= HW_ADC_INPUTS_PER_CHANNEL)
            {
                ret = false;
            }
        }
    }

    // [impl->fw~hal_adc_007~1]
    if (ret)
    {
        channelData->multimodeApplied = channelConfig->configureMultimode;
    }

    // Register one SIL input port per enabled, named input: the framework may
    // drive its pin voltage (volts, native units); undriven inputs keep the
    // synthetic ramp. Null-safe: with no hooks installed every handle stays
    // invalid.
    if (ret)
    {
        for (uint8_t input = 0U; input < HW_ADC_INPUTS_PER_CHANNEL; input++)
        {
            if ((channelConfig->inputs[input].enabled) &&
                (channelConfig->inputs[input].inputNameStr != NULL))
            {
                channelData->portHandles[input] =
                    SIL_ports_register("vsig", channelConfig->inputs[input].inputNameStr, "V");
            }
        }
    }

    // [impl->fw~hal_adc_010~1]
    // Link the DMA channel's completion callback.
    if (ret && regularDMA && (numEnabledRegular > 0U))
    {
        if (channelConfig->dmaChannel < HW_DMA_CHANNEL_COUNT)
        {
            ret &= HW_DMA_registerCallback(channelConfig->dmaChannel,
                                           HW_ADC_private_dmaXferComplete,
                                           (void *)(uintptr_t)channel);
        }
        else
        {
            // Config typo: reject at init per the guard contract above.
            ret = false;
        }
    }

    // Cache derived state for run-time use.
    if (ret)
    {
        channelData->numEnabledInputs         = numEnabledRegular;
        channelData->numEnabledInjectedInputs = numEnabledInjected;
        for (uint8_t r = 0U; r < numEnabledRegular; r++)
        {
            channelData->rankOrder[r] = rankOrder[r];
        }
        channelData->numBits = channelConfig->numBits;
    }

    return ret;
}

// [impl->fw~hal_adc_003~1]
// Arm the hardware-triggered injected path: BUSY until the first completion,
// and one TRGO sink serves every injected channel (the modeled trigger line
// fans out, as TRGO2 does). Runs only once the driver is in service.
static bool HW_ADC_private_armTriggeredChannels(void)
{
    bool ret = true;
    for (size_t channel = 0U; channel < data->config->numChannels; channel++)
    {
        const HW_ADC_channelConfig_S * const channelConfig = &data->config->channels[channel];
        if (ret && (data->channelData[channel].numEnabledInjectedInputs > 0U))
        {
            data->channelData[channel].injectedConversionStatus = HW_ADC_CONVERSION_STATUS_BUSY;
            ret = HW_TIM_registerTrgoCallback(HW_ADC_triggerPeripheralMapping[channelConfig->injectedTrigger],
                                              HW_ADC_private_trgoHandler, NULL);
            if (HW_ADC_completionIrqHandle == SIL_IRQ_HANDLE_INVALID)
            {
                HW_ADC_completionIrqHandle = SIL_irq_registerPended(HW_ADC_sim_completionDispatch,
                                                                    HW_ADC_IRQ_PRIORITY);
            }
        }
    }
    return ret;
}

// The sim twin of the DMA channel's transfer-complete vector.
static void HW_ADC_private_dmaXferComplete(HW_DMA_channel_E dmaChannel, void * context)
{
    if (data->initialized)
    {
        const HW_ADC_channels_E channel = (HW_ADC_channels_E)(uintptr_t)context;

        if (channel < HW_ADC_CHANNEL_COUNT)
        {
            HW_ADC_channelData_S * const channelData = &data->channelData[channel];

            if (!channelData->discardTransfer)
            {
                const bool ok = (HW_DMA_getStatus(dmaChannel) == HW_DMA_STATUS_COMPLETE);

                channelData->status = (ok) ? HW_ADC_CONVERSION_STATUS_OK : HW_ADC_CONVERSION_STATUS_FAULT;

                if (ok)
                {
                    // The bytes the transfer moved: the sim DMA has no data
                    // register to read, so the sampled stream lands here.
                    for (uint8_t r = 0U; r < channelData->numEnabledInputs; r++)
                    {
                        channelData->dmaDestinationBuffer[r] = channelData->dmaSourceBuffer[r];
                    }

                    // map rank order conversions into their IN# indexed .counts buffer
                    for (uint8_t r = 0U; r < channelData->numEnabledInputs; r++)
                    {
                        const uint8_t input = channelData->rankOrder[r];
                        channelData->counts[input] = channelData->dmaDestinationBuffer[r];
                    }
                    channelData->countsValid = true;
                }

                if (channelData->conversionCompleteCallback != NULL)
                {
                    channelData->conversionCompleteCallback(channel,
                                                            channelData->status,
                                                            channelData->conversionCompleteCallbackContext);
                }
            }
        }
    }
}

// [impl->fw~hal_adc_003~1]
// TRGO sink: fires once per trigger event during HW_TIM_advanceTime
// (platform-tick context). Samples every injected channel wired to this
// peripheral whose edge select accepts the crossing, then pends the
// completion interrupt, the NVIC twin of JEOS.
static void HW_ADC_private_trgoHandler(HW_TIM_peripheral_E peripheral, HW_TIM_trgoCross_E cross, void * context)
{
    (void)context;
    if (data->initialized)
    {
        for (size_t channel = 0U; channel < data->config->numChannels; channel++)
        {
            const HW_ADC_channelConfig_S * const channelConfig = &data->config->channels[channel];
            HW_ADC_channelData_S * const channelData = &data->channelData[channel];
            if ((channelData->numEnabledInjectedInputs == 0U) ||
                (HW_ADC_triggerPeripheralMapping[channelConfig->injectedTrigger] != peripheral))
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
            // (shared with the regular path, as on silicon).
            for (uint8_t i = 0U; i < channelData->numEnabledInjectedInputs; i++)
            {
                const uint8_t  pin        = channelConfig->injectedInputs[i].pinInput;
                const uint32_t rampOffset = ((uint32_t)channel * 256U) + HW_ADC_INJECTED_RAMP_BASE + ((uint32_t)i * 16U);
                channelData->injectedCounts[i] = HW_ADC_private_sampleInput((HW_ADC_channels_E)channel, pin, rampOffset);
            }
            channelData->pendingCompletions++;
            SIL_irq_pend(HW_ADC_completionIrqHandle);
        }
    }
}

// [impl->fw~hal_adc_008~1]
// Injected completion interrupt (the JEOS ISR's twin): pended by the trigger
// handler, dispatched in the firmware fiber during the same step's ISR phase.
// Drains every pending conversion, one status write + callback per trigger
// event, so the completion count matches the trigger cadence on any grid.
void HW_ADC_sim_completionDispatch(void)
{
    if (data->initialized)
    {
        for (size_t channel = 0U; channel < HW_ADC_CHANNEL_COUNT; channel++)
        {
            HW_ADC_channelData_S * const channelData = &data->channelData[channel];
            while (channelData->pendingCompletions > 0U)
            {
                channelData->pendingCompletions--;
                channelData->injectedConversionStatus = HW_ADC_CONVERSION_STATUS_OK;
                if (channelData->injectedConversionCompleteCallback != NULL)
                {
                    channelData->injectedConversionCompleteCallback((HW_ADC_channels_E)channel,
                                                                    channelData->injectedConversionStatus,
                                                                    channelData->injectedConversionCompleteCallbackContext);
                }
            }
        }
    }
}

// [impl->fw~hal_adc_004~1]
static void HW_ADC_private_startPolledRegularConversion(HW_ADC_channels_E channel)
{
    HW_ADC_channelData_S * const channelData = &data->channelData[channel];
    HW_ADC_conversionStatus_E status = HW_ADC_CONVERSION_STATUS_OK;

    // A stalled channel models a poll timeout: counts untouched, pass faulted.
    if (channelData->conversionStall)
    {
        status = HW_ADC_CONVERSION_STATUS_FAULT;
    }
    else
    {
        for (uint8_t r = 0U; r < channelData->numEnabledInputs; r++)
        {
            const uint8_t  input      = channelData->rankOrder[r];
            const uint32_t rampOffset = ((uint32_t)channel * 256U) + ((uint32_t)input * 16U);
            channelData->counts[input] = HW_ADC_private_sampleInput(channel, input, rampOffset);
        }
        channelData->countsValid = true;
    }

    channelData->status = status;
}

// [impl->fw~hal_adc_009~1]
static void HW_ADC_private_startDmaRegularConversion(HW_ADC_channels_E channel)
{
    const HW_ADC_channelConfig_S * const channelConfig = &data->config->channels[channel];
    HW_ADC_channelData_S * const channelData = &data->channelData[channel];
    HW_ADC_conversionStatus_E status = HW_ADC_CONVERSION_STATUS_IDLE;

    // Prior pass's sequence incomplete: record the fault;
    // counts keep their last-completed-pass values.
    const bool dmaXferFailedToComplete = (channelData->status == HW_ADC_CONVERSION_STATUS_BUSY);
    if (dmaXferFailedToComplete)
    {
        channelData->discardTransfer = true;
        (void)HW_DMA_abortTransfer(channelConfig->dmaChannel);
        status = HW_ADC_CONVERSION_STATUS_FAULT;
    }
    else // start the next periodic read
    {
        // The conversion instant: sample the sequence into the data-register
        // stream the completion lands in dmaDestinationBuffer.
        for (uint8_t r = 0U; r < channelData->numEnabledInputs; r++)
        {
            const uint8_t  input      = channelData->rankOrder[r];
            const uint32_t rampOffset = ((uint32_t)channel * 256U) + ((uint32_t)input * 16U);
            channelData->dmaSourceBuffer[r] = (uint16_t)HW_ADC_private_sampleInput(channel, input, rampOffset);
        }

        // A fresh transfer re-arms the completion; only the overlap path above
        // discards one.
        channelData->discardTransfer = false;
        const bool started = HW_DMA_startTransfer(channelConfig->dmaChannel,
                                                  channelData->dmaDestinationBuffer,
                                                  channelData->numEnabledInputs);
        if (started)
        {
            status = HW_ADC_CONVERSION_STATUS_BUSY;
        }
        else
        {
            status = HW_ADC_CONVERSION_STATUS_FAULT;
            (void)HW_DMA_abortTransfer(channelConfig->dmaChannel);
        }
    }

    channelData->status = status;
}

/* Public Function Definitions */

// [impl->fw~hal_adc_001~1]
bool HW_ADC_init(const HW_ADC_config_S * const config)
{
    bool ret = false;

    // Drop out of service before touching config or handles; the previous
    // completion service goes with it, so a re-init is a clean slate.
    SIL_irq_cancel(HW_ADC_completionIrqHandle);
    HW_ADC_completionIrqHandle = SIL_IRQ_HANDLE_INVALID;
    *data = (HW_ADC_data_S){ 0 };
    for (size_t channel = 0U; channel < HW_ADC_CHANNEL_COUNT; channel++)
    {
        for (uint8_t input = 0U; input < HW_ADC_INPUTS_PER_CHANNEL; input++)
        {
            data->channelData[channel].portHandles[input] = SIL_PORTS_HANDLE_INVALID;
        }
    }

    if ((config != NULL) &&
        (config->channels != NULL) &&
        (config->numChannels <= HW_ADC_CHANNEL_COUNT))
    {
        data->config = config;

        bool success = true;
        for (size_t channel = 0U; channel < config->numChannels; channel++)
        {
            if (!HW_ADC_private_initOneChannel((HW_ADC_channels_E)channel))
            {
                success = false;
                break;
            }
        }

        if (success)
        {
            data->initialized = true;
            if (HW_ADC_private_armTriggeredChannels())
            {
                ret = true;
            }
            else
            {
                data->initialized = false;
            }
        }
    }
    return ret;
}

// The injected sequence never rides this pass: it is armed from init and
// completes in its completion interrupt.
void HW_ADC_run1ms(void)
{
    if (data->initialized)
    {
        data->tickCounter++;

        for (size_t channel = 0U; channel < data->config->numChannels; channel++)
        {
            const HW_ADC_channelConfig_S * const channelConfig = &data->config->channels[channel];
            const uint8_t numEnabledRegular = data->channelData[channel].numEnabledInputs;

            // Regular-sequence polled path.
            // [impl->fw~hal_adc_004~1]
            if ((channelConfig->xferMode == HW_ADC_XFER_POLLED) &&
                (numEnabledRegular > 0U))
            {
                HW_ADC_private_startPolledRegularConversion((HW_ADC_channels_E)channel);
            }

            // Regular-sequence DMA path. Start the sequence; the DMA completion
            // callback scatters counts[] and publishes OK/FAULT, so this task
            // owns the status only up to the start.
            // [impl->fw~hal_adc_009~1]
            if ((channelConfig->xferMode == HW_ADC_XFER_DMA) &&
                (numEnabledRegular > 0U))
            {
                HW_ADC_private_startDmaRegularConversion((HW_ADC_channels_E)channel);
            }
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
        (data->config->channels[channel].inputs[inputIndex].enabled) &&
        (data->channelData[channel].countsValid))
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
            const uint32_t maxCounts = (1UL << data->channelData[channel].numBits) - 1UL;

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
            const uint32_t maxCounts = (1UL << data->channelData[channel].numBits) - 1UL;

            *out = ((float32_t)counts / (float32_t)maxCounts) * channelConfig->vref;
            ret = true;
        }
    }
    return ret;
}

// [impl->fw~hal_adc_008~1]
bool HW_ADC_registerCallback(HW_ADC_channels_E channel,
                             HW_ADC_callback_F callback,
                             void * context)
{
    bool ret = false;
    if ((data->initialized) &&
        (channel < HW_ADC_CHANNEL_COUNT))
    {
        // may be NULL
        data->channelData[channel].conversionCompleteCallback = callback;
        data->channelData[channel].conversionCompleteCallbackContext = context;
        ret = true;
    }
    return ret;
}

// [impl->fw~hal_adc_008~1]
bool HW_ADC_registerInjectedCallback(HW_ADC_channels_E channel,
                                     HW_ADC_callback_F callback,
                                     void * context)
{
    bool ret = false;
    if ((data->initialized) &&
        (channel < HW_ADC_CHANNEL_COUNT))
    {
        // may be NULL
        data->channelData[channel].injectedConversionCompleteCallback = callback;
        data->channelData[channel].injectedConversionCompleteCallbackContext = context;
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
        *out = data->channelData[channel].injectedConversionStatus;
        ret = true;
    }
    return ret;
}

// [impl->fw~hal_adc_008~1]
bool HW_ADC_getErrorCount(HW_ADC_channels_E channel, uint32_t * const out)
{
    bool ret = false;
    if ((data->initialized) &&
        (channel < HW_ADC_CHANNEL_COUNT) &&
        (out != NULL))
    {
        *out = data->channelData[channel].errorCount;
        ret = true;
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
        *out = data->channelData[channel].status;
        ret = true;
    }
    return ret;
}
