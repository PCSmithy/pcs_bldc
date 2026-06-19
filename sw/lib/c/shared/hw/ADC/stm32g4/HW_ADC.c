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
static bool    HW_ADC_private_initOneChannel(HW_ADC_channels_E ch);

/* Private Data Definitions */

static HW_ADC_data_S HW_ADC_data;
static HW_ADC_data_S * const data = &HW_ADC_data;

// Injected ranks, like regular ranks, are JSQR register-field encodings
// (ADC_INJECTED_RANK_1 == 9, not 1), so the driver indexes the HAL
// constants by dense sequence position rather than using a literal ordinal.
// Used both to configure (HAL_ADCEx_InjectedConfigChannel) and to read back
// (HAL_ADCEx_InjectedGetValue).
static const uint32_t HW_ADC_injectedRankConstants[HW_ADC_INJECTED_INPUTS_PER_CHANNEL] =
{
    ADC_INJECTED_RANK_1, ADC_INJECTED_RANK_2, ADC_INJECTED_RANK_3, ADC_INJECTED_RANK_4,
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

static bool HW_ADC_private_initOneChannel(HW_ADC_channels_E ch)
{
    bool ret = true;
    const HW_ADC_channelConfig_S * const channelConfig = &data->config->channels[ch];

    // Interim guard: only software-triggered + polled is built, so reject
    // any other trigger/xfer mode at init rather than silently producing
    // empty count buffers. This is not a spec behavior — fw~hal_adc_003
    // requires every mode to initialize — so the guard carries no impl
    // tag and falls away as the timer, interrupt, and DMA paths land.
    if ((channelConfig->triggerMode         != HW_ADC_TRIGGER_SOFTWARE) ||
        (channelConfig->xferMode            != HW_ADC_XFER_POLLED)      ||
        (channelConfig->injectedTriggerMode != HW_ADC_TRIGGER_SOFTWARE) ||
        (channelConfig->injectedXferMode    != HW_ADC_XFER_POLLED))
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
        data->channelData[ch].numEnabledInputs         = 0U;
        data->channelData[ch].numEnabledInjectedInputs = 0U;
    }

    // Copy hadc to mutable storage; apply library-managed Init overrides
    // for the regular path. (Injected config doesn't touch hadc.Init —
    // those overrides happen per-input via HAL_ADCEx_InjectedConfigChannel.)
    if ((ret) && (needsHALInit))
    {
        data->channelData[ch].hadc = channelConfig->hadc;
        if (numEnabledRegular > 0U)
        {
            data->channelData[ch].hadc.Init.NbrOfConversion    = numEnabledRegular;
            data->channelData[ch].hadc.Init.ScanConvMode       = ((numEnabledRegular > 1U)) ? ADC_SCAN_ENABLE : ADC_SCAN_DISABLE;
            data->channelData[ch].hadc.Init.EOCSelection       = ADC_EOC_SINGLE_CONV;
            data->channelData[ch].hadc.Init.ContinuousConvMode = DISABLE;
        }
        if (channelConfig->triggerMode == HW_ADC_TRIGGER_SOFTWARE)
        {
            data->channelData[ch].hadc.Init.ExternalTrigConv = ADC_SOFTWARE_START;
        }

        if (HAL_ADC_Init(&data->channelData[ch].hadc) != HAL_OK)
        {
            ret = false;
        }
    }

    // Calibrate. Single-ended; covers both regular and injected paths
    // (calibration is a peripheral-level operation on STM32G4).
    if ((ret) && (needsHALInit))
    {
        if (HAL_ADCEx_Calibration_Start(&data->channelData[ch].hadc, ADC_SINGLE_ENDED) != HAL_OK)
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
                &data->channelData[ch].hadc,
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
                if (HAL_ADC_ConfigChannel(&data->channelData[ch].hadc, &sConfig) != HAL_OK)
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
            if (channelConfig->injectedTriggerMode == HW_ADC_TRIGGER_SOFTWARE)
            {
                iConfig.ExternalTrigInjecConv = ADC_INJECTED_SOFTWARE_START;
            }
            if (HAL_ADCEx_InjectedConfigChannel(&data->channelData[ch].hadc, &iConfig) != HAL_OK)
            {
                ret = false;
                break;
            }
        }
    }

    // Cache derived state for run-time use.
    if ((ret) && (needsHALInit))
    {
        data->channelData[ch].numEnabledInputs         = numEnabledRegular;
        data->channelData[ch].numEnabledInjectedInputs = numEnabledInjected;
        for (uint8_t r = 0U; r < numEnabledRegular; r++)
        {
            data->channelData[ch].rankOrder[r] = rankOrder[r];
        }
        data->channelData[ch].numBits = HW_ADC_private_resolutionToNumBits(channelConfig->hadc.Init.Resolution);
    }

    return ret;
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
        for (size_t ch = 0U; ch < config->numChannels; ch++)
        {
            if (!HW_ADC_private_initOneChannel((HW_ADC_channels_E)ch))
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
        for (size_t ch = 0U; ch < data->config->numChannels; ch++)
        {
            const HW_ADC_channelConfig_S * const channelConfig = &data->config->channels[ch];
            HW_ADC_conversionStatus_E status = HW_ADC_CONVERSION_STATUS_IDLE;

            // Regular-sequence path. DMA / interrupt-driven channels
            // populate counts[] outside of _run1ms; only POLLED needs
            // per-tick service.
            // [impl->fw~hal_adc_004~1]
            const uint8_t numEnabledRegular = data->channelData[ch].numEnabledInputs;
            if ((channelConfig->xferMode == HW_ADC_XFER_POLLED) &&
                (numEnabledRegular > 0U))
            {
                status = HW_ADC_CONVERSION_STATUS_OK;
                if (HAL_ADC_Start(&data->channelData[ch].hadc) == HAL_OK)
                {
                    for (uint8_t r = 0U; r < numEnabledRegular; r++)
                    {
                        if (HAL_ADC_PollForConversion(&data->channelData[ch].hadc, HW_ADC_POLL_TIMEOUT_MS) != HAL_OK)
                        {
                            // Timed out: record the fault, leave remaining counts stale.
                            status = HW_ADC_CONVERSION_STATUS_FAULT;
                            break;
                        }
                        const uint8_t input = data->channelData[ch].rankOrder[r];
                        data->channelData[ch].counts[input] = HAL_ADC_GetValue(&data->channelData[ch].hadc);
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
            const uint8_t numEnabledInjected = data->channelData[ch].numEnabledInjectedInputs;
            if ((channelConfig->injectedXferMode == HW_ADC_XFER_POLLED) &&
                (numEnabledInjected > 0U))
            {
                if (status == HW_ADC_CONVERSION_STATUS_IDLE)
                {
                    status = HW_ADC_CONVERSION_STATUS_OK;
                }
                if (HAL_ADCEx_InjectedStart(&data->channelData[ch].hadc) == HAL_OK)
                {
                    // HAL_ADCEx_InjectedPollForConversion waits for the
                    // entire injected sequence to complete (JEOS), then
                    // we read all values from the JDR registers.
                    if (HAL_ADCEx_InjectedPollForConversion(&data->channelData[ch].hadc, HW_ADC_POLL_TIMEOUT_MS) == HAL_OK)
                    {
                        for (uint8_t r = 0U; r < numEnabledInjected; r++)
                        {
                            data->channelData[ch].injectedCounts[r] =
                                HAL_ADCEx_InjectedGetValue(&data->channelData[ch].hadc, HW_ADC_injectedRankConstants[r]);
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

            data->channelData[ch].status = status;
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
