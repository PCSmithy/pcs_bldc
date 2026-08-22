/* Includes */

#include "lib_types.h"

#include "HW_ADC.h"
#include "stm32g4xx_hal.h"

/* Defines */

// HAL_ADC_PollForConversion timeout. Per-conversion polled wait at
// fast sampling on the G4 is sub-microsecond, so a few ms of headroom
// is essentially infinite while still bounding the worst case if the
// peripheral hangs.
#define HW_ADC_POLL_TIMEOUT_MS    (2U)

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
    // matches user's inputs[] indexing).
    uint32_t counts[HW_ADC_INPUTS_PER_CHANNEL];

    // Injected-sequence results, indexed by sequence position (dense,
    // matches user's injectedInputs[] indexing). Slot N holds the value
    // from injected rank N+1.
    uint32_t injectedCounts[HW_ADC_INJECTED_INPUTS_PER_CHANNEL];

    // Outcome of the most recent _run1ms pass on this channel.
    HW_ADC_conversionStatus_E status;

    HW_ADC_conversionStatus_E injectedConversionStatus;
    HW_ADC_injectedCallback_F injectedConversionCompleteCallback;
    void * injectedConversionCompleteCallbackContext;

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
static HW_ADC_channels_E HW_ADC_private_channelFromHandle(ADC_HandleTypeDef * hadc);

/* Private Data Definitions */

static HW_ADC_data_S HW_ADC_data;
static HW_ADC_data_S * const data = &HW_ADC_data;

static const uint32_t HW_ADC_injectedRankConstants[HW_ADC_INJECTED_INPUTS_PER_CHANNEL] =
{
    ADC_INJECTED_RANK_1, ADC_INJECTED_RANK_2, ADC_INJECTED_RANK_3, ADC_INJECTED_RANK_4,
};

static const uint32_t HW_ADC_injectedExternalTriggerMapping[HW_ADC_TIMER_TRIGGER_COUNT] =
{
    [HW_ADC_TIMER_TRIGGER_TIM1_TRGO2] = ADC_EXTERNALTRIGINJEC_T1_TRGO2,
};

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
    static const uint32_t rankConstants[HW_ADC_INPUTS_PER_CHANNEL] =
    {
        ADC_REGULAR_RANK_1,  ADC_REGULAR_RANK_2,  ADC_REGULAR_RANK_3,  ADC_REGULAR_RANK_4,
        ADC_REGULAR_RANK_5,  ADC_REGULAR_RANK_6,  ADC_REGULAR_RANK_7,  ADC_REGULAR_RANK_8,
        ADC_REGULAR_RANK_9,  ADC_REGULAR_RANK_10, ADC_REGULAR_RANK_11, ADC_REGULAR_RANK_12,
        ADC_REGULAR_RANK_13, ADC_REGULAR_RANK_14, ADC_REGULAR_RANK_15, ADC_REGULAR_RANK_16,
    };

    bool found = false;
    for (uint8_t i = 0U; i < HW_ADC_INPUTS_PER_CHANNEL; i++)
    {
        if (rankConstants[i] == rankConstant)
        {
            *ordinal = i + 1U;   // 1..16
            found    = true;
            break;
        }
    }
    return found;
}

static bool HW_ADC_private_initOneChannel(HW_ADC_channels_E channel)
{
    bool ret = true;
    const HW_ADC_channelConfig_S * const channelConfig = &data->config->channels[channel];

    // Feature guard: reject trigger/transfer combinations the driver has not
    // built, rather than silently misconfiguring. Regular: software + polled
    // only. Injected: software + polled, or the timer-triggered interrupt
    // path with a mapped trigger source.
    if ((channelConfig->triggerMode != HW_ADC_TRIGGER_SOFTWARE) ||
        (channelConfig->xferMode    != HW_ADC_XFER_POLLED))
    {
        ret = false;
    }

    const bool injectedPolled    = ((channelConfig->injectedTriggerMode == HW_ADC_TRIGGER_SOFTWARE) &&
                                    (channelConfig->injectedXferMode    == HW_ADC_XFER_POLLED));
    const bool injectedTriggered = ((channelConfig->injectedTriggerMode == HW_ADC_TRIGGER_TIMER) &&
                                    (channelConfig->injectedXferMode    == HW_ADC_XFER_INTERRUPT) &&
                                    (channelConfig->injectedTimerTrigger < HW_ADC_TIMER_TRIGGER_COUNT));
    if ((!injectedPolled) && (!injectedTriggered))
    {
        ret = false;
    }

    // [impl->fw~hal_adc_002~1]
    // Walk the sparse regular inputs[] array, count enabled inputs, and
    // build the rank-ordered IN# list. Each enabled input carries a HAL
    // rank constant (sConfig.Rank) that we map to its 1..N ordinal. The
    // enabled ordinals must form a contiguous 1..N with no gaps or
    // duplicates, else the HAL sequence (NbrOfConversion = N) and our
    // rankOrder[] mapping silently disagree. rankOrder[o-1] = the IN# slot
    // that occupies sequence ordinal o.
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

    // No-op success: peripheral listed but nothing enabled on either path.
    const bool needsHALInit = ((ret) && ((numEnabledRegular > 0U) || (numEnabledInjected > 0U)));
    if ((ret) && (!needsHALInit))
    {
        data->channelData[channel].numEnabledInputs         = 0U;
        data->channelData[channel].numEnabledInjectedInputs = 0U;
    }

    // Copy hadc to mutable storage; apply library-managed Init overrides
    // for the regular path. (Injected config doesn't touch hadc.Init —
    // those overrides happen per-input via HAL_ADCEx_InjectedConfigChannel.)
    if ((ret) && (needsHALInit))
    {
        data->channelData[channel].hadc = channelConfig->hadc;
        if (numEnabledRegular > 0U)
        {
            data->channelData[channel].hadc.Init.NbrOfConversion    = numEnabledRegular;
            data->channelData[channel].hadc.Init.ScanConvMode       = ((numEnabledRegular > 1U)) ? ADC_SCAN_ENABLE : ADC_SCAN_DISABLE;
            data->channelData[channel].hadc.Init.EOCSelection       = ADC_EOC_SINGLE_CONV;
            data->channelData[channel].hadc.Init.ContinuousConvMode = DISABLE;

            // Polled multi-rank scans read DR one rank at a time from a task
            // loop far slower than the back-to-back conversions. Auto-delayed
            // conversion (AUTDLY) halts the sequencer after each conversion
            // until HAL_ADC_GetValue reads DR, so EOC can't coalesce and later
            // ranks can't overrun the preserved DR — the per-rank poll+read
            // stays correct at any CPU/poll speed. Without it the 2nd poll
            // waits on an EOC that never fires again (sequence already done).
            if (channelConfig->xferMode == HW_ADC_XFER_POLLED)
            {
                data->channelData[channel].hadc.Init.LowPowerAutoWait = ENABLE;
            }
        }
        switch (channelConfig->triggerMode)
        {
            case HW_ADC_TRIGGER_SOFTWARE:
                data->channelData[channel].hadc.Init.ExternalTrigConv = ADC_SOFTWARE_START;
                break;
            case HW_ADC_TRIGGER_TIMER:
            default:
                // TODO - support TIM trigger for regular conversions - map HW_ADC_timerTrigger_E to EXTSEL[4:0] to set ExternalTrigConv
                break;

        }
        if (HAL_ADC_Init(&data->channelData[channel].hadc) != HAL_OK)
        {
            ret = false;
        }
    }

    // Calibrate. Single-ended; covers both regular and injected paths
    // (calibration is a peripheral-level operation on STM32G4).
    if ((ret) && (needsHALInit))
    {
        if (HAL_ADCEx_Calibration_Start(&data->channelData[channel].hadc, ADC_SINGLE_ENDED) != HAL_OK)
        {
            ret = false;
        }
    }

    // [impl->fw~hal_adc_007~1]
    if ((ret) && (needsHALInit) && (channelConfig->configureMultimode))
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
    if ((ret) && (numEnabledRegular > 0U))
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

    // Configure each enabled injected input. Library overrides
    // InjectedRank (from array position), InjectedNbrOfConversion (from
    // total count), and ExternalTrigInjecConv (from injectedTriggerMode);
    // user supplies channel + sampling time + the rest.
    //
    // Reference: ODrive uses these same HAL_ADCEx_Injected* APIs for
    // FOC current sensing on STM32F405 — see
    //   https://github.com/odriverobotics/ODrive
    // (Firmware/MotorControl/ contains the timer-triggered + ISR setup
    // we'd model the eventual TIMER+INTERRUPT path on.) ST's MC SDK
    // (X-CUBE-MCSDK) is the other canonical reference but its source
    // isn't on a public GitHub.
    if ((ret) && (numEnabledInjected > 0U))
    {
        for (uint8_t r = 0U; r < numEnabledInjected; r++)
        {
            ADC_InjectionConfTypeDef iConfig = channelConfig->injectedInputs[r].sConfig;
            iConfig.InjectedRank            = HW_ADC_injectedRankConstants[r];
            iConfig.InjectedNbrOfConversion = numEnabledInjected;

            // [impl->fw~hal_adc_003~1]
            switch (channelConfig->injectedTriggerMode)
            {
                default:
                case HW_ADC_TRIGGER_SOFTWARE:
                    iConfig.ExternalTrigInjecConv = ADC_INJECTED_SOFTWARE_START;
                    break;
                case HW_ADC_TRIGGER_TIMER:
                    iConfig.ExternalTrigInjecConv = HW_ADC_injectedExternalTriggerMapping[channelConfig->injectedTimerTrigger];
                    break;
            }

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
    if ((ret) && (needsHALInit))
    {
        data->channelData[channel].numEnabledInputs         = numEnabledRegular;
        data->channelData[channel].numEnabledInjectedInputs = numEnabledInjected;
        for (uint8_t r = 0U; r < numEnabledRegular; r++)
        {
            data->channelData[channel].rankOrder[r] = rankOrder[r];
        }
        data->channelData[channel].numBits = HW_ADC_private_resolutionToNumBits(channelConfig->hadc.Init.Resolution);
    }

    // [impl->fw~hal_adc_003~1]
    // Arm the timer-triggered interrupt path; software + polled injected is
    // driven from run1ms instead.
    if ((ret) && (numEnabledInjected > 0U) &&
        (channelConfig->injectedXferMode == HW_ADC_XFER_INTERRUPT))
    {
        data->channelData[channel].injectedConversionStatus = HW_ADC_CONVERSION_STATUS_BUSY;
        ret &= HAL_ADCEx_InjectedStart_IT(&data->channelData[channel].hadc) == HAL_OK;
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
            ret = true;
        }
    }
    return ret;
}

void HW_ADC_run1ms(void)
{
    if (data->initialized)
    {
        for (size_t channel = 0U; channel < data->config->numChannels; channel++)
        {
            const HW_ADC_channelConfig_S * const channelConfig = &data->config->channels[channel];
            HW_ADC_conversionStatus_E status = HW_ADC_CONVERSION_STATUS_IDLE;

            // Regular-sequence path. DMA / interrupt-driven channels
            // populate counts[] outside of _run1ms; only POLLED needs
            // per-tick service.
            // [impl->fw~hal_adc_004~1]
            const uint8_t numEnabledRegular = data->channelData[channel].numEnabledInputs;
            if ((channelConfig->xferMode == HW_ADC_XFER_POLLED) &&
                (numEnabledRegular > 0U))
            {
                status = HW_ADC_CONVERSION_STATUS_OK;
                if (HAL_ADC_Start(&data->channelData[channel].hadc) == HAL_OK)
                {
                    for (uint8_t r = 0U; r < numEnabledRegular; r++)
                    {
                        if (HAL_ADC_PollForConversion(&data->channelData[channel].hadc, HW_ADC_POLL_TIMEOUT_MS) != HAL_OK)
                        {
                            // Timed out: record the fault, leave remaining counts stale.
                            status = HW_ADC_CONVERSION_STATUS_FAULT;
                            break;
                        }
                        const uint8_t input = data->channelData[channel].rankOrder[r];
                        data->channelData[channel].counts[input] = HAL_ADC_GetValue(&data->channelData[channel].hadc);
                    }
                    // ContinuousConvMode is DISABLE, so the peripheral stops itself
                    // when the sequence completes. No HAL_ADC_Stop needed.
                }
                else
                {
                    status = HW_ADC_CONVERSION_STATUS_FAULT;
                }
            }

            // Injected-sequence path. Same SW+POLLED gating; injected
            // preempts the regular sequence in hardware, but since we
            // run them sequentially here that's a non-issue. ISR/DMA
            // injected (the FOC use case) won't go through _run1ms.
            // [impl->fw~hal_adc_006~1]
            const uint8_t numEnabledInjected = data->channelData[channel].numEnabledInjectedInputs;
            if ((channelConfig->injectedXferMode == HW_ADC_XFER_POLLED) &&
                (numEnabledInjected > 0U))
            {
                if (status == HW_ADC_CONVERSION_STATUS_IDLE)
                {
                    status = HW_ADC_CONVERSION_STATUS_OK;
                }
                if (HAL_ADCEx_InjectedStart(&data->channelData[channel].hadc) == HAL_OK)
                {
                    // HAL_ADCEx_InjectedPollForConversion waits for the
                    // entire injected sequence to complete (JEOS), then
                    // we read all values from the JDR registers.
                    if (HAL_ADCEx_InjectedPollForConversion(&data->channelData[channel].hadc, HW_ADC_POLL_TIMEOUT_MS) == HAL_OK)
                    {
                        for (uint8_t r = 0U; r < numEnabledInjected; r++)
                        {
                            data->channelData[channel].injectedCounts[r] =
                                HAL_ADCEx_InjectedGetValue(&data->channelData[channel].hadc, HW_ADC_injectedRankConstants[r]);
                        }
                    }
                    else
                    {
                        status = HW_ADC_CONVERSION_STATUS_FAULT;
                    }
                }
                else
                {
                    status = HW_ADC_CONVERSION_STATUS_FAULT;
                }
            }

            data->channelData[channel].status = status;
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
bool HW_ADC_registerInjectedCallback(HW_ADC_channels_E channel,
                                     HW_ADC_injectedCallback_F callback,
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
