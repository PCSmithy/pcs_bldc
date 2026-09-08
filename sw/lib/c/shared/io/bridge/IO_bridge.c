/* Includes */
#include "lib_types.h"
#include "IO_bridge.h"
#include "HW_TIM.h"

#include "IO_bridge_channels.h"

/* Defines */

/* Typedefs */

typedef struct
{
    HW_TIM_peripheral_E moePeripheral;

    float32_t current_amps[IO_BRIDGE_PHASE_COUNT];
    uint32_t  sampleTime_us[IO_BRIDGE_PHASE_COUNT];
    uint32_t  updateCount[IO_BRIDGE_PHASE_COUNT];

} IO_bridge_channelData_S;


typedef struct
{
    const IO_bridge_config_S * config;

    IO_bridge_channelData_S channels[IO_BRIDGE_CHANNEL_COUNT];
} IO_bridge_data_S;

/* Private Data Definitions */

static IO_bridge_data_S IO_bridge_data;
static IO_bridge_data_S * const data = &IO_bridge_data;

static const IO_bridge_phase_E IO_bridge_complementaryPhase[IO_BRIDGE_PHASE_COUNT] =
{
    [IO_BRIDGE_PHASE_U] = IO_BRIDGE_PHASE_V,
    [IO_BRIDGE_PHASE_V] = IO_BRIDGE_PHASE_U,
    [IO_BRIDGE_PHASE_W] = IO_BRIDGE_PHASE_COUNT,
};

/* Private Function Declarations */

static HW_TIM_channels_E IO_bridge_private_phaseChannel(const IO_bridge_channelConfig_S * const channelConfig, IO_bridge_phase_E phase);
static uint32_t IO_bridge_private_dutyToCompare(float32_t duty, uint32_t period);
static bool IO_bridge_private_decodeCurrent(const IO_bridge_currentSenseConfig_S * const sense, float32_t volts, float32_t * const amps_out);
static bool IO_bridge_private_readCurrent(const IO_bridge_currentSenseConfig_S * const sense, float32_t * const amps_out);
static bool IO_bridge_private_readInjectedCurrent(const IO_bridge_currentSenseConfig_S * const sense, float32_t * const amps_out);
static void IO_bridge_private_completeInjectedPair(size_t channel, uint32_t now);
static void IO_bridge_private_injectedComplete(HW_ADC_channels_E channel, HW_ADC_conversionStatus_E status, void * context);
static bool IO_bridge_private_registerInjected(const IO_bridge_config_S * const config);

/* Private Function Definitions */

static HW_TIM_channels_E IO_bridge_private_phaseChannel(
    const IO_bridge_channelConfig_S * const channelConfig, IO_bridge_phase_E phase)
{
    HW_TIM_channels_E timChannel = channelConfig->phaseU;
    if (phase == IO_BRIDGE_PHASE_V)
    {
        timChannel = channelConfig->phaseV;
    }
    else if (phase == IO_BRIDGE_PHASE_W)
    {
        timChannel = channelConfig->phaseW;
    }
    else
    {
        // IO_BRIDGE_PHASE_U (or a rejected value the caller already guarded).
    }
    return timChannel;
}

// Round duty x period to the nearest count.
static uint32_t IO_bridge_private_dutyToCompare(float32_t duty, uint32_t period)
{
    return (uint32_t)((duty * (float32_t)period) + 0.5f);
}

static bool IO_bridge_private_decodeCurrent(const IO_bridge_currentSenseConfig_S * const sense, float32_t volts, float32_t * const amps_out)
{
    bool ret = false;
    if (sense->voltsPerAmp != 0.0f)
    {
        *amps_out = (volts - sense->zeroCurrentBias_V) / sense->voltsPerAmp;
        ret = true;
    }
    return ret;
}

static bool IO_bridge_private_readCurrent(const IO_bridge_currentSenseConfig_S * const sense, float32_t * const amps_out)
{
    bool ret = false;
    float32_t volts = 0.0f;
    if (HW_ADC_getVolts(sense->adcChannel, sense->adcInput, &volts))
    {
        ret = IO_bridge_private_decodeCurrent(sense, volts, amps_out);
    }
    return ret;
}

// Same conversion off the sense's injected sequence slot (the PWM-crest sample).
static bool IO_bridge_private_readInjectedCurrent(const IO_bridge_currentSenseConfig_S * const sense, float32_t * const amps_out)
{
    bool ret = false;
    float32_t volts = 0.0f;
    if (HW_ADC_getInjectedVolts(sense->adcChannel, sense->injectedIndex, &volts))
    {
        ret = IO_bridge_private_decodeCurrent(sense, volts, amps_out);
    }
    return ret;
}

// U and V are sampled simultaneously - derive W from them by KCL
static void IO_bridge_private_completeInjectedPair(size_t channel, uint32_t now)
{
    IO_bridge_channelData_S * const channelData = &data->channels[channel];
    channelData->current_amps[IO_BRIDGE_PHASE_W] = -(channelData->current_amps[IO_BRIDGE_PHASE_U] +
                                                        channelData->current_amps[IO_BRIDGE_PHASE_V]);
    channelData->updateCount[IO_BRIDGE_PHASE_W] += 1U;
    channelData->sampleTime_us[IO_BRIDGE_PHASE_W] = now;

}


static void IO_bridge_private_injectedComplete(HW_ADC_channels_E adcChannel, HW_ADC_conversionStatus_E status, void * context)
{
    (void)context;

    // A failed conversion leaves the last good sample standing
    if ((data->config != NULL) &&
        (status == HW_ADC_CONVERSION_STATUS_OK))
    {
        uint32_t now_us = 0U;

        // Without a time base the pair window is meaningless: every stamp would
        // read zero and every sample would look simultaneous.
        if (HW_TIM_getCounter(data->config->timeBasePeripheral, &now_us))
        {
            for (size_t channel = 0U; channel < data->config->numChannels; channel++)
            {
                const IO_bridge_channelConfig_S * const channelConfig = &data->config->channels[channel];
                IO_bridge_channelData_S * const channelData = &data->channels[channel];

                for (uint8_t phase = 0U; phase < IO_BRIDGE_PHASE_COUNT; phase++)
                {
                    const IO_bridge_currentSenseConfig_S * const sense = &channelConfig->phaseCurrent[phase];
                    if ((sense->adcChannel == adcChannel) &&
                        (sense->injectedIndex != IO_BRIDGE_INJECTED_NONE))
                    {
                        float32_t amps = 0.0f;
                        if (IO_bridge_private_readInjectedCurrent(sense, &amps))
                        {
                            channelData->current_amps[phase] = amps;
                            channelData->updateCount[phase] += 1U;
                            channelData->sampleTime_us[phase] = now_us;

                            const IO_bridge_phase_E partner = IO_bridge_complementaryPhase[phase];
                            if (partner < IO_BRIDGE_PHASE_COUNT) // don't think I need this check because we're already within a `if (sense->injectedIndex != IO_BRIDGE_INJECTED_NONE)` block
                            {
                                // Unsigned subtract is wrap-safe; the partner's stamp is always in the past.
                                const uint32_t timeSincePartner_us = now_us - channelData->sampleTime_us[partner];
                                if ((channelData->updateCount[partner] != 0U) &&
                                    (timeSincePartner_us <= channelConfig->injectedPairWindow_us))
                                {
                                    IO_bridge_private_completeInjectedPair(channel, now_us);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

// Hang the completion callback on every ADC channel carrying an injected phase
// sense, once per channel however many phases share it.
static bool IO_bridge_private_registerInjected(const IO_bridge_config_S * const config)
{
    bool ret = true;
    bool registered[HW_ADC_CHANNEL_COUNT] = { false };

    for (size_t channel = 0U; (channel < config->numChannels) && ret; channel++)
    {
        const IO_bridge_channelConfig_S * const channelConfig = &config->channels[channel];

        for (uint8_t phase = 0U; (phase < IO_BRIDGE_PHASE_COUNT) && ret; phase++)
        {
            const IO_bridge_currentSenseConfig_S * const sense = &channelConfig->phaseCurrent[phase];

            if (sense->injectedIndex != IO_BRIDGE_INJECTED_NONE)
            {
                if ((sense->adcChannel < HW_ADC_CHANNEL_COUNT) &&
                    (sense->injectedIndex < HW_ADC_INJECTED_INPUTS_PER_CHANNEL))
                {
                    if (!registered[sense->adcChannel])
                    {
                        ret = HW_ADC_registerInjectedCallback(sense->adcChannel,
                                                                IO_bridge_private_injectedComplete,
                                                                NULL);
                        registered[sense->adcChannel] = ret;
                    }
                }
                else
                {
                    ret = false;
                }
            }
        }
    }

    return ret;
}

/* Public Function Definitions */

// [impl->fw~io_bridge_001~1]
bool IO_bridge_init(const IO_bridge_config_S * const config)
{
    bool success = false;

    if ((config != NULL) &&
        (config->channels != NULL) &&
        (config->numChannels <= IO_BRIDGE_CHANNEL_COUNT))
    {
        success = true;
        for (size_t channel = 0U; (channel < config->numChannels) && success; channel++)
        {
            const IO_bridge_channelConfig_S * const channelConfig = &config->channels[channel];
            const HW_TIM_channels_E phaseU = channelConfig->phaseU;
            const HW_TIM_channels_E phaseV = channelConfig->phaseV;
            const HW_TIM_channels_E phaseW = channelConfig->phaseW;

            HW_TIM_peripheral_E periphU = (HW_TIM_peripheral_E)0;
            HW_TIM_peripheral_E periphV = (HW_TIM_peripheral_E)0;
            HW_TIM_peripheral_E periphW = (HW_TIM_peripheral_E)0;

            // getPeripheral fails on an out-of-range phase channel; requiring
            // all three to share one peripheral makes its MOE the whole-bridge
            // gate.
            if ((HW_TIM_getPeripheral(phaseU, &periphU)) &&
                (HW_TIM_getPeripheral(phaseV, &periphV)) &&
                (HW_TIM_getPeripheral(phaseW, &periphW)) &&
                (periphU == periphV) &&
                (periphV == periphW))
            {
                data->channels[channel].moePeripheral = periphU;

                // Enable each phase's output-compare unit up front; outputs stay
                // dark because HW_TIM commands MOE off at init, leaving the
                // shared master output enable as the sole runtime gate.
                success = (HW_TIM_setOutputEnabled(phaseU, true) &&
                           HW_TIM_setOutputEnabled(phaseV, true) &&
                           HW_TIM_setOutputEnabled(phaseW, true));
            }
            else
            {
                success = false;
            }
        }

        if (success)
        {
            // HW_ADC_init armed the injected group before this runs, so a few
            // conversions complete un-consumed; the driver keeps their counts.
            success = IO_bridge_private_registerInjected(config);
        }

        if (success)
        {
            data->config = config;
        }
    }

    return success;
}

// [impl->fw~io_bridge_002~1]
bool IO_bridge_setPhaseDuty(IO_bridge_channel_E channel, IO_bridge_phase_E phase, float32_t duty01)
{
    bool ret = false;

    if ((data->config != NULL) &&
        (channel < IO_BRIDGE_CHANNEL_COUNT) &&
        ((size_t)channel < data->config->numChannels) &&
        (phase < IO_BRIDGE_PHASE_COUNT) &&
        (duty01 >= 0.0f) &&
        (duty01 <= 1.0f))
    {
        const HW_TIM_channels_E timChannel =
            IO_bridge_private_phaseChannel(&data->config->channels[channel], phase);
        uint32_t period = 0U;
        if (HW_TIM_getPeriod(timChannel, &period))
        {
            const uint32_t compare = IO_bridge_private_dutyToCompare(duty01, period);
            ret = HW_TIM_setCompare(timChannel, compare);
        }
    }

    return ret;
}

// [impl->fw~io_bridge_004~1]
bool IO_bridge_setPhaseOutputEnabled(IO_bridge_channel_E channel, IO_bridge_phase_E phase, bool enabled)
{
    bool ret = false;

    if ((data->config != NULL) &&
        (channel < IO_BRIDGE_CHANNEL_COUNT) &&
        ((size_t)channel < data->config->numChannels) &&
        (phase < IO_BRIDGE_PHASE_COUNT))
    {
        const HW_TIM_channels_E timChannel =
            IO_bridge_private_phaseChannel(&data->config->channels[channel], phase);
        ret = HW_TIM_setOutputEnabled(timChannel, enabled);
    }

    return ret;
}

// [impl->fw~io_bridge_003~1]
bool IO_bridge_setOutputEnabled(IO_bridge_channel_E channel, bool enabled)
{
    bool ret = false;

    if ((data->config != NULL) &&
        (channel < IO_BRIDGE_CHANNEL_COUNT) &&
        ((size_t)channel < data->config->numChannels))
    {
        ret = HW_TIM_setMainOutputEnabled(data->channels[channel].moePeripheral, enabled);
    }

    return ret;
}

// [impl->fw~io_bridge_003~1]
bool IO_bridge_getOutputEnabled(IO_bridge_channel_E channel, bool * const enabled)
{
    bool ret = false;

    if ((data->config != NULL) &&
        (enabled != NULL) &&
        (channel < IO_BRIDGE_CHANNEL_COUNT) &&
        ((size_t)channel < data->config->numChannels))
    {
        ret = HW_TIM_getMainOutputEnabled(data->channels[channel].moePeripheral, enabled);
    }

    return ret;
}

bool IO_bridge_clearBreakFlags(IO_bridge_channel_E channel)
{
    bool ret = false;

    if ((data->config != NULL) &&
        (channel < IO_BRIDGE_CHANNEL_COUNT) &&
        ((size_t)channel < data->config->numChannels))
    {
        ret = HW_TIM_clearBreakFlags(data->channels[channel].moePeripheral);
    }

    return ret;
}

bool IO_bridge_getPhaseCurrent(IO_bridge_channel_E channel, IO_bridge_phase_E phase, float32_t * const amps_out)
{
    bool ret = false;

    if ((data->config != NULL) &&
        (amps_out != NULL) &&
        (channel < IO_BRIDGE_CHANNEL_COUNT) &&
        ((size_t)channel < data->config->numChannels) &&
        (phase < IO_BRIDGE_PHASE_COUNT))
    {
        ret = IO_bridge_private_readCurrent(
            &data->config->channels[channel].phaseCurrent[phase], amps_out);
    }

    return ret;
}

bool IO_bridge_getBusCurrent(IO_bridge_channel_E channel, float32_t * const amps_out)
{
    bool ret = false;

    if ((data->config != NULL) &&
        (amps_out != NULL) &&
        (channel < IO_BRIDGE_CHANNEL_COUNT) &&
        ((size_t)channel < data->config->numChannels))
    {
        ret = IO_bridge_private_readCurrent(
            &data->config->channels[channel].busCurrent, amps_out);
    }

    return ret;
}

bool IO_bridge_getInjectedPhaseCurrent(IO_bridge_channel_E channel, IO_bridge_phase_E phase, float32_t * const amps_out)
{
    bool ret = false;

    if ((data->config != NULL) &&
        (amps_out != NULL) &&
        (channel < IO_BRIDGE_CHANNEL_COUNT) &&
        ((size_t)channel < data->config->numChannels) &&
        (phase < IO_BRIDGE_PHASE_COUNT) &&
        (data->channels[channel].updateCount[phase] != 0U))
    {
        *amps_out = data->channels[channel].current_amps[phase];
        ret = true;
    }

    return ret;
}

bool IO_bridge_getInjectedUpdateCount(IO_bridge_channel_E channel, IO_bridge_phase_E phase, uint32_t * const out)
{
    bool ret = false;

    if ((data->config != NULL) &&
        (out != NULL) &&
        (channel < IO_BRIDGE_CHANNEL_COUNT) &&
        ((size_t)channel < data->config->numChannels) &&
        (phase < IO_BRIDGE_PHASE_COUNT))
    {
        *out = data->channels[channel].updateCount[phase];
        ret = true;
    }

    return ret;
}
