/* Includes */

#include "lib_types.h"

#include "HW_ADC.h"
#include "stm32g4xx_hal.h"

/* Defines */

// HAL_ADC_PollForConversion timeout. Per-conversion polled wait at
// fast sampling on the G4 is sub-microsecond, so a 1 ms of headroom
// is essentially infinite while still bounding the worst case if the
// peripheral hangs.
#define HW_ADC_POLL_TIMEOUT_MS    (1U)

// Alias the HAL InjectedConvCpltCallback onto our canonical naming convention
#define HW_ADC_private_injectedConversionSequenceCompleteCallback HAL_ADCEx_InjectedConvCpltCallback
#define HW_ADC_private_errorCallback HAL_ADC_ErrorCallback

/* Typedefs */

typedef struct
{
    // Library-owned mutable HAL handles (HAL_ADC_* mutates state inside
    // these). Initialized by copying from the user's const config.
    ADC_HandleTypeDef hadc;

    // Per-channel derived state.
    uint8_t numEnabledInputs;
    uint8_t numEnabledInjectedInputs;

    // rankOrder[r] = physical IN# that occupies regular-sequence rank
    // (r+1). Built at init by walking the sparse inputs[] array; used at
    // run time to map "Nth conversion in the sequence" -> "which counts[]
    // slot to store it in".
    uint8_t rankOrder[HW_ADC_INPUTS_PER_CHANNEL];

    // Decoded resolution (bits) for the volts conversion.
    uint8_t numBits;

    // Regular-sequence results, indexed by physical IN# (sparse storage,
    // matches user's inputs[] indexing). Undefined until the channel's first
    // completed pass sets countsValid; reads refuse them before that.
    uint32_t counts[HW_ADC_INPUTS_PER_CHANNEL];
    bool countsValid;

    // indexed in Rank order
    uint16_t dmaDestinationBuffer[HW_ADC_INPUTS_PER_CHANNEL];

    // Injected-sequence results, indexed by sequence position (dense,
    // matches user's injectedInputs[] indexing). Slot N holds the value
    // from injected rank N+1.
    uint32_t injectedCounts[HW_ADC_INJECTED_INPUTS_PER_CHANNEL];

    // Outcome of the most recent _run1ms pass on this channel.
    HW_ADC_conversionStatus_E status;
    HW_ADC_callback_F conversionCompleteCallback;
    void * conversionCompleteCallbackContext;

    HW_ADC_conversionStatus_E injectedConversionStatus;
    HW_ADC_callback_F injectedConversionCompleteCallback;
    void * injectedConversionCompleteCallbackContext;

    // Total HAL error-callback edges on this channel; never latches, never
    // clears except at init.
    uint32_t errorCount;

    // flag to discard any output from a latent callback after a hung DMA transfer
    bool discardTransfer;

} HW_ADC_channelData_S;

typedef struct
{
    const HW_ADC_config_S * config;

    HW_ADC_channelData_S channelData[HW_ADC_CHANNEL_COUNT];

    bool initialized;
} HW_ADC_data_S;

/* Private Function Declarations */

static uint8_t HW_ADC_private_resolutionToNumBits(uint32_t resolution);
static bool    HW_ADC_private_rankToOrdinal(uint32_t rankConstant, uint8_t * const ordinal);
static bool    HW_ADC_private_initOneChannel(HW_ADC_channels_E channel);
static bool    HW_ADC_private_armTriggeredChannels(void);
static HW_ADC_channels_E HW_ADC_private_channelFromHandle(ADC_HandleTypeDef * hadc);
static void HW_ADC_private_dmaXferComplete(HW_DMA_channel_E dmaChannel, void * context);

/* Private Data Definitions */

static HW_ADC_data_S HW_ADC_data;
static HW_ADC_data_S * const data = &HW_ADC_data;

static const uint32_t HW_ADC_injectedRankConstants[HW_ADC_INJECTED_INPUTS_PER_CHANNEL] =
{
    ADC_INJECTED_RANK_1, ADC_INJECTED_RANK_2, ADC_INJECTED_RANK_3, ADC_INJECTED_RANK_4,
};

static const uint32_t HW_ADC_injectedExternalTriggerMapping[HW_ADC_INJECTED_TRIGGER_COUNT] =
{
    [HW_ADC_INJECTED_TRIGGER_TIM1_TRGO2] = ADC_EXTERNALTRIGINJEC_T1_TRGO2,
};

/* Inline Private Function Definitions */

static inline void HW_ADC_private_startPolledRegularConversion(const HW_ADC_channelConfig_S * const channelConfig, HW_ADC_channelData_S * const channelData)
{
    (void)channelConfig;
    HW_ADC_conversionStatus_E status = HW_ADC_CONVERSION_STATUS_OK;

    // The injected JEOS ISR read-modify-writes State/ErrorCode on
    // this same handle and takes no HAL lock; BASEPRI cannot mask
    // it, so bracket the start's own read-modify-write.
    __disable_irq();
    const HAL_StatusTypeDef startStatus = HAL_ADC_Start(&channelData->hadc);
    __enable_irq();

    if (startStatus == HAL_OK)
    {
        for (uint8_t r = 0U; r < channelData->numEnabledInputs; r++)
        {
            if (HAL_ADC_PollForConversion(&channelData->hadc, HW_ADC_POLL_TIMEOUT_MS) != HAL_OK)
            {
                // Timed out: record the fault, leave remaining counts stale.
                status = HW_ADC_CONVERSION_STATUS_FAULT;
                break;
            }
            const uint8_t input = channelData->rankOrder[r];
            channelData->counts[input] = HAL_ADC_GetValue(&channelData->hadc);
        }
        if (status == HW_ADC_CONVERSION_STATUS_OK)
        {
            channelData->countsValid = true;
        }
        // ContinuousConvMode is DISABLE, so the peripheral stops itself
        // when the sequence completes. No HAL_ADC_Stop needed.
    }
    else
    {
        status = HW_ADC_CONVERSION_STATUS_FAULT;
    }

    channelData->status = status;
}

static inline void HW_ADC_private_startDmaRegularConversion(const HW_ADC_channelConfig_S * const channelConfig, HW_ADC_channelData_S * const channelData)
{
    ADC_HandleTypeDef * const hadc = &channelData->hadc;

    // Prior pass's sequence incomplete: record the fault;
    // counts keep their last-completed-pass values.
    __disable_irq();
    const bool dmaXferFailedToComplete = (channelData->status == HW_ADC_CONVERSION_STATUS_BUSY);
    if (dmaXferFailedToComplete)
    {
        channelData->discardTransfer = true;
    }
    __enable_irq();

    if (dmaXferFailedToComplete)
    {
        HW_DMA_abortTransfer(channelConfig->dmaChannel);
        LL_ADC_REG_StopConversion(hadc->Instance);
        channelData->status = HW_ADC_CONVERSION_STATUS_FAULT;
    }
    else // start the next periodic read
    {
        // Replicates HAL_ADC_Start_DMA's handle/flag prep (completion
        // rides the HW_DMA callback instead of the HAL's). The whole
        // prep is bracketed: the injected JEOS ISR read-modify-writes
        // State/ErrorCode on this same handle and takes no HAL lock.
        __disable_irq();
        ADC_STATE_CLR_SET(hadc->State,
                    HAL_ADC_STATE_READY | HAL_ADC_STATE_REG_EOC | HAL_ADC_STATE_REG_OVR | HAL_ADC_STATE_REG_EOSMP | HAL_ADC_STATE_MULTIMODE_SLAVE,
                    HAL_ADC_STATE_REG_BUSY);
        if ((hadc->State & HAL_ADC_STATE_INJ_BUSY) != 0UL)
        {
            CLEAR_BIT(hadc->ErrorCode, (HAL_ADC_ERROR_OVR | HAL_ADC_ERROR_DMA));
        }
        else
        {
            ADC_CLEAR_ERRORCODE(hadc);
        }
        __HAL_ADC_CLEAR_FLAG(hadc, (ADC_FLAG_EOC | ADC_FLAG_EOS | ADC_FLAG_OVR));
        __HAL_UNLOCK(hadc);
        __HAL_ADC_ENABLE_IT(hadc, ADC_IT_OVR);

        // A fresh transfer re-arms the completion; only the overlap path above
        // discards one.
        channelData->discardTransfer = false;
        const bool started = HW_DMA_startTransfer(channelConfig->dmaChannel,
                                                    channelData->dmaDestinationBuffer,
                                                    channelData->numEnabledInputs);
        if (started)
        {
            // BUSY is published before the start so the completion
            // ISR's OK/FAULT is never overwritten by this task.
            channelData->status = HW_ADC_CONVERSION_STATUS_BUSY;
            LL_ADC_REG_StartConversion(hadc->Instance);
        }
        __enable_irq();

        if (!started)
        {
            channelData->status = HW_ADC_CONVERSION_STATUS_FAULT;
            (void)HW_DMA_abortTransfer(channelConfig->dmaChannel);
        }
    }
}

/* Private Function Definitions */

static uint8_t HW_ADC_private_resolutionToNumBits(uint32_t resolution)
{
    switch (resolution)
    {
        case ADC_RESOLUTION_12B: return 12U;
        case ADC_RESOLUTION_10B: return 10U;
        case ADC_RESOLUTION_8B:  return 8U;
        case ADC_RESOLUTION_6B:  return 6U;
        default:                 return 12U;
    }
}

// The HAL regular-rank constants (ADC_REGULAR_RANK_n) are SQRx register-
// field encodings, not the ordinals 1..16 — e.g. RANK_1 == 6, RANK_2 == 12,
// RANK_3 == 18, RANK_6 == 262. Map a config's HAL rank constant back to its
// 1..N ordinal so the driver can order and validate the sequence. Returns
// false if the value isn't a recognized regular-rank constant.
static bool HW_ADC_private_rankToOrdinal(uint32_t rankConstant, uint8_t * const ordinal)
{
    bool found = false;

#define HW_ADC_RANK_ORDINAL_CASE(n) \
    case ADC_REGULAR_RANK_##n: \
        *ordinal = (uint8_t)(n); \
        found = true; \
        break; \

    switch (rankConstant)
    {
        HW_ADC_RANK_ORDINAL_CASE(1) HW_ADC_RANK_ORDINAL_CASE(2) HW_ADC_RANK_ORDINAL_CASE(3) HW_ADC_RANK_ORDINAL_CASE(4)
        HW_ADC_RANK_ORDINAL_CASE(5) HW_ADC_RANK_ORDINAL_CASE(6) HW_ADC_RANK_ORDINAL_CASE(7) HW_ADC_RANK_ORDINAL_CASE(8)
        HW_ADC_RANK_ORDINAL_CASE(9) HW_ADC_RANK_ORDINAL_CASE(10) HW_ADC_RANK_ORDINAL_CASE(11) HW_ADC_RANK_ORDINAL_CASE(12)
        HW_ADC_RANK_ORDINAL_CASE(13) HW_ADC_RANK_ORDINAL_CASE(14) HW_ADC_RANK_ORDINAL_CASE(15) HW_ADC_RANK_ORDINAL_CASE(16)

        default:
            break;
    }
#undef HW_ADC_RANK_ORDINAL_CASE

    return found;
}

static bool HW_ADC_private_initOneChannel(HW_ADC_channels_E channel)
{
    bool ret = true;
    const HW_ADC_channelConfig_S * const channelConfig = &data->config->channels[channel];
    ADC_HandleTypeDef * const hadc = &data->channelData[channel].hadc;

    data->channelData[channel].errorCount = 0U;

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

    // [impl->fw~hal_adc_002~1]
    // Walk the sparse regular inputs[] array, count enabled inputs, and
    // build the rank-ordered IN# list. Channel rank list must be contiguous
    uint8_t numEnabledRegular = 0U;
    uint8_t rankOrder[HW_ADC_INPUTS_PER_CHANNEL] = { 0 };
    bool    ordinalSeen[HW_ADC_INPUTS_PER_CHANNEL + 1U] = { false };
    if (ret)
    {
        for (uint8_t input = 0U; input < HW_ADC_INPUTS_PER_CHANNEL; input++)
        {
            if (channelConfig->inputs[input].enabled)
            {
                uint8_t ordinal = 0U;
                if ((!HW_ADC_private_rankToOrdinal(channelConfig->inputs[input].sConfig.Rank, &ordinal)) ||
                    (ordinalSeen[ordinal]))
                {
                    ret = false;
                    break;
                }
                ordinalSeen[ordinal]    = true;
                rankOrder[ordinal - 1U] = input;
                numEnabledRegular++;
            }
        }
    }

    // Enabled ordinals must be contiguous 1..numEnabledRegular (no gaps).
    if (ret)
    {
        for (uint8_t ordinal = 1U; ordinal <= numEnabledRegular; ordinal++)
        {
            if (!ordinalSeen[ordinal])
            {
                ret = false;
                break;
            }
        }
    }

    // Count enabled injected inputs. Storage is dense: slot N is rank
    // N+1. Enforce contiguous-from-zero (sparse like [0]=true, [2]=true
    // would silently skip slot 1's HAL config and mis-rank everything).
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
                    // Gap in the sequence — reject.
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
    // an enabled sequence needs a known trigger source.
    if (ret && (numEnabledInjected > 0U) &&
        (channelConfig->injectedTrigger >= HW_ADC_INJECTED_TRIGGER_COUNT))
    {
        ret = false;
    }

    // No-op success: peripheral listed but nothing enabled on either path.
    const bool needsHALInit = (ret && ((numEnabledRegular > 0U) || (numEnabledInjected > 0U)));
    if (ret && !needsHALInit)
    {
        data->channelData[channel].numEnabledInputs         = 0U;
        data->channelData[channel].numEnabledInjectedInputs = 0U;
    }

    // Copy hadc to mutable storage; apply library-managed Init overrides
    // for the regular path. (Injected config doesn't touch hadc.Init —
    // those overrides happen per-input via HAL_ADCEx_InjectedConfigChannel.)
    if (ret && needsHALInit)
    {
        data->channelData[channel].hadc = channelConfig->hadc;
        if (numEnabledRegular > 0U)
        {
            hadc->Init.NbrOfConversion    = numEnabledRegular;
            hadc->Init.ScanConvMode       = ((numEnabledRegular > 1U)) ? ADC_SCAN_ENABLE : ADC_SCAN_DISABLE;
            hadc->Init.EOCSelection       = ADC_EOC_SINGLE_CONV;
            hadc->Init.ContinuousConvMode = DISABLE;

            // AUTDLY holds the sequencer until DR is read: without it the HAL clears
            // EOC and EOS together and a preempted rank reads its successor's value.
            if (channelConfig->xferMode == HW_ADC_XFER_POLLED)
            {
                hadc->Init.LowPowerAutoWait = ENABLE;
            }

            if (channelConfig->xferMode == HW_ADC_XFER_DMA)
            {
                hadc->Init.DMAContinuousRequests = ENABLE;
            }
        }
        switch (channelConfig->triggerMode)
        {
            case HW_ADC_TRIGGER_SOFTWARE:
                hadc->Init.ExternalTrigConv = ADC_SOFTWARE_START;
                break;
            case HW_ADC_TRIGGER_HARDWARE:
            default:
                // TODO - hardware-triggered regular conversions: map a trigger
                // source enum to EXTSEL[4:0] to set ExternalTrigConv
                break;

        }
        if (HAL_ADC_Init(&data->channelData[channel].hadc) != HAL_OK)
        {
            ret = false;
        }
    }

    // Calibrate. Single-ended; covers both regular and injected paths
    // (calibration is a peripheral-level operation on STM32G4).
    if (ret && needsHALInit)
    {
        if (HAL_ADCEx_Calibration_Start(&data->channelData[channel].hadc, ADC_SINGLE_ENDED) != HAL_OK)
        {
            ret = false;
        }
    }

    // [impl->fw~hal_adc_007~1]
    if (ret && needsHALInit && (channelConfig->configureMultimode))
    {
        // HAL signature is non-const; the user's multimode struct is
        // const, hence the cast. Read-only access in practice.
        if (HAL_ADCEx_MultiModeConfigChannel(
                &data->channelData[channel].hadc,
                (ADC_MultiModeTypeDef *)&channelConfig->multimode) != HAL_OK)
        {
            ret = false;
        }
    }

    // Configure each enabled regular input's sequence rank.
    if (ret && (numEnabledRegular > 0U))
    {
        for (uint8_t input = 0U; input < HW_ADC_INPUTS_PER_CHANNEL; input++)
        {
            if (channelConfig->inputs[input].enabled)
            {
                ADC_ChannelConfTypeDef sConfig = channelConfig->inputs[input].sConfig;
                if (HAL_ADC_ConfigChannel(&data->channelData[channel].hadc, &sConfig) != HAL_OK)
                {
                    ret = false;
                    break;
                }
            }
        }
    }

    // [impl->fw~hal_adc_010~1]
    // Link the DMA channel and set the ADC's DMA request mode. Unlimited
    // requests (DMAEN + DMACFG) because the request-stop latch of one-shot
    // mode cannot be cleared once the injected group holds JADSTART; both
    // bits are writable here (ADSTART/JADSTART stay 0 until
    // HW_ADC_private_armTriggeredChannels). The ADC is enabled now so a
    // channel with no injected inputs can still convert.
    if (ret && regularDMA && (numEnabledRegular > 0U))
    {
        if (channelConfig->dmaChannel < HW_DMA_CHANNEL_COUNT)
        {
            ret &= HW_DMA_registerCallback(channelConfig->dmaChannel,
                                            HW_ADC_private_dmaXferComplete,
                                            (void *)(uintptr_t)channel);
            LL_ADC_REG_SetDMATransfer(hadc->Instance, LL_ADC_REG_DMA_TRANSFER_UNLIMITED);
            ret &= (ADC_Enable(hadc) == HAL_OK);
        }
        else
        {
            // Config typo: reject at init per the guard contract above.
            ret = false;
        }
    }

    // Configure each enabled injected input. Library overrides
    // InjectedRank (from array position), InjectedNbrOfConversion (from
    // total count), and ExternalTrigInjecConv (from injectedTrigger);
    // user supplies channel + sampling time + the rest.
    //
    // Reference: ODrive uses these same HAL_ADCEx_Injected* APIs for
    // FOC current sensing on STM32F405 — see
    //   https://github.com/odriverobotics/ODrive
    if (ret && (numEnabledInjected > 0U))
    {
        for (uint8_t r = 0U; r < numEnabledInjected; r++)
        {
            ADC_InjectionConfTypeDef iConfig = channelConfig->injectedInputs[r].sConfig;
            iConfig.InjectedRank            = HW_ADC_injectedRankConstants[r];
            iConfig.InjectedNbrOfConversion = numEnabledInjected;

            // [impl->fw~hal_adc_003~1]
            iConfig.ExternalTrigInjecConv     = HW_ADC_injectedExternalTriggerMapping[channelConfig->injectedTrigger];
            iConfig.ExternalTrigInjecConvEdge = (channelConfig->injectedTriggerEdge == HW_ADC_TRIGGER_EDGE_RISING)
                                                    ? ADC_EXTERNALTRIGINJECCONV_EDGE_RISING
                                                    : ADC_EXTERNALTRIGINJECCONV_EDGE_FALLING;

            if (HAL_ADCEx_InjectedConfigChannel(&data->channelData[channel].hadc, &iConfig) != HAL_OK)
            {
                ret = false;
                break;
            }
        }
        data->channelData[channel].injectedConversionCompleteCallback = NULL;
    }

    // Cache derived state for run-time use.
    if (ret && needsHALInit)
    {
        data->channelData[channel].numEnabledInputs         = numEnabledRegular;
        data->channelData[channel].numEnabledInjectedInputs = numEnabledInjected;
        for (uint8_t r = 0U; r < numEnabledRegular; r++)
        {
            data->channelData[channel].rankOrder[r] = rankOrder[r];
        }
        data->channelData[channel].numBits = HW_ADC_private_resolutionToNumBits(channelConfig->hadc.Init.Resolution);
    }

    return ret;
}

// [impl->fw~hal_adc_003~1]
// Arm the hardware-triggered injected path. Runs only once the driver is in
// service, so a trigger landing immediately is serviced rather than dropped by
// the ISR's initialized guard.
static bool HW_ADC_private_armTriggeredChannels(void)
{
    bool ret = true;
    for (size_t channel = 0U; channel < data->config->numChannels; channel++)
    {
        if (ret && (data->channelData[channel].numEnabledInjectedInputs > 0U))
        {
            data->channelData[channel].injectedConversionStatus = HW_ADC_CONVERSION_STATUS_BUSY;
            ret = (HAL_ADCEx_InjectedStart_IT(&data->channelData[channel].hadc) == HAL_OK);
            if (ret)
            {
                // Start_IT arms per-conversion JEOC when EOCSelection is
                // SINGLE_CONV (the regular path's setting); this driver's
                // contract is sequence-complete, so arm JEOS instead.
                __HAL_ADC_DISABLE_IT(&data->channelData[channel].hadc, ADC_IT_JEOC);
                __HAL_ADC_ENABLE_IT(&data->channelData[channel].hadc, ADC_IT_JEOS);
            }
        }
    }
    return ret;
}

static HW_ADC_channels_E HW_ADC_private_channelFromHandle(ADC_HandleTypeDef * hadc)
{
    HW_ADC_channels_E channel = HW_ADC_CHANNEL_COUNT;
    for (size_t ch = 0U; ch < data->config->numChannels; ch++)
    {
        if (data->channelData[ch].hadc.Instance == hadc->Instance)
        {
            channel = ch;
            break;
        }
    }
    return channel;
}

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

// Called by HAL
// [impl->fw~hal_adc_008~1]
void HW_ADC_private_injectedConversionSequenceCompleteCallback(ADC_HandleTypeDef * hadc)
{
    if (data->initialized)
    {
        const HW_ADC_channels_E channel = HW_ADC_private_channelFromHandle(hadc);
        if (channel < HW_ADC_CHANNEL_COUNT)
        {
            HW_ADC_channelData_S * const channelData = &data->channelData[channel];

            for (uint8_t i = 0U; i < channelData->numEnabledInjectedInputs; i++)
            {
                channelData->injectedCounts[i] = HAL_ADCEx_InjectedGetValue(hadc, HW_ADC_injectedRankConstants[i]);
            }
            channelData->injectedConversionStatus = HW_ADC_CONVERSION_STATUS_OK;

            if (channelData->injectedConversionCompleteCallback != NULL)
            {
                channelData->injectedConversionCompleteCallback(channel,
                                                                channelData->injectedConversionStatus,
                                                                channelData->injectedConversionCompleteCallbackContext);
            }
        }
    }
}

// [impl->fw~hal_adc_008~1]
void HW_ADC_private_errorCallback(ADC_HandleTypeDef * hadc)
{
    if (data->initialized)
    {
        const HW_ADC_channels_E channel = HW_ADC_private_channelFromHandle(hadc);
        if (channel < HW_ADC_CHANNEL_COUNT)
        {
            HW_ADC_channelData_S * const channelData = &data->channelData[channel];

            channelData->errorCount++;

            // OVR/DMA error codes belong to the regular group; anything else
            // (internal, injected queue overflow) faults the injected path.
            if ((hadc->ErrorCode & (HAL_ADC_ERROR_OVR | HAL_ADC_ERROR_DMA)) != 0UL)
            {
                channelData->status = HW_ADC_CONVERSION_STATUS_FAULT;
                if (channelData->conversionCompleteCallback != NULL)
                {
                    channelData->conversionCompleteCallback(channel,
                                                            channelData->status,
                                                            channelData->conversionCompleteCallbackContext);
                }
            }
            else
            {
                channelData->injectedConversionStatus = HW_ADC_CONVERSION_STATUS_FAULT;
                if (channelData->injectedConversionCompleteCallback != NULL)
                {
                    channelData->injectedConversionCompleteCallback(channel,
                                                                    channelData->injectedConversionStatus,
                                                                    channelData->injectedConversionCompleteCallbackContext);
                }
            }
        }
    }
}

void HW_ADC_irqHandler(void)
{
    if (data->initialized)
    {
        for (size_t channel = 0U; channel < data->config->numChannels; channel++)
        {
            // Only handles HAL_ADC_Init ran on (a channel with no enabled
            // inputs never gets one; its Instance stays NULL).
            if (data->channelData[channel].hadc.Instance != NULL)
            {
                HAL_ADC_IRQHandler(&data->channelData[channel].hadc);
            }
        }
    }
}

/* Public Function Definitions */

// [impl->fw~hal_adc_001~1]
bool HW_ADC_init(const HW_ADC_config_S * const config)
{
    bool ret = false;

    // Drop out of service before touching config or handles: the injected ISR
    // guards on initialized and would otherwise run against half-swapped state.
    *data = (HW_ADC_data_S){ 0 };

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
// completes in the JEOS ISR.
void HW_ADC_run1ms(void)
{
    if (data->initialized)
    {
        for (size_t channel = 0U; channel < data->config->numChannels; channel++)
        {
            const HW_ADC_channelConfig_S * const channelConfig = &data->config->channels[channel];
            HW_ADC_channelData_S * const channelData = &data->channelData[channel];

            const uint8_t numEnabledRegular = channelData->numEnabledInputs;

            // Regular-sequence polled path. Trigger conversions and poll for result
            // [impl->fw~hal_adc_004~1]
            if ((channelConfig->xferMode == HW_ADC_XFER_POLLED) &&
                (numEnabledRegular > 0U))
            {
                HW_ADC_private_startPolledRegularConversion(channelConfig, channelData);
            }

            // Regular-sequence DMA path. Start the sequence; the DMA completion
            // callback scatters counts[] and publishes OK/FAULT, so this task
            // owns the status only up to the start.
            // [impl->fw~hal_adc_009~1]
            if ((channelConfig->xferMode == HW_ADC_XFER_DMA) &&
                (numEnabledRegular > 0U))
            {
                HW_ADC_private_startDmaRegularConversion(channelConfig, channelData);
            }
        }
    }
}

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

// [impl->fw~conn_trace_008~1]
void HW_ADC_setInjectedIrqMasked(bool masked)
{
    // The injected line's NVIC number is board-assigned (HAL MSP), so the mask
    // is global; the regions it brackets are a handful of instructions.
    if (masked)
    {
        __disable_irq();
    }
    else
    {
        __enable_irq();
    }
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
