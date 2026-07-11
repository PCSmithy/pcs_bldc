/* Includes */
#include "IO_bridge.h"
#include "HW_TIM.h"

#include "IO_bridge_channels.h"

/* Defines */

/* Typedefs */

typedef struct
{
    const IO_bridge_config_S * config;
    // The HW_TIM peripheral whose master output enable gates each bridge,
    // derived at init from the bridge's three phases (all share one peripheral).
    HW_TIM_peripheral_E moePeripheral[IO_BRIDGE_CHANNEL_COUNT];
} IO_bridge_data_S;

/* Private Data Definitions */

static IO_bridge_data_S IO_bridge_data;
static IO_bridge_data_S * const data = &IO_bridge_data;

/* Private Function Declarations */

static HW_TIM_channels_E IO_bridge_private_phaseChannel(
    const IO_bridge_channelConfig_S * const channelConfig, IO_bridge_phase_E phase);
static uint32_t IO_bridge_private_dutyToCompare(float32_t duty, uint32_t period);
static bool IO_bridge_private_readCurrent(
    const IO_bridge_currentSenseConfig_S * const sense, float32_t * const amps_out);

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

// Round duty x period to the nearest count. duty is pre-validated to [0, 1], so
// the result is bounded by period and needs no clamp. duty 1 maps to exactly
// the period (HW_TIM accepts compare == period); center-aligned PWM1 therefore
// falls one tick short of a true 100% at the counter peak, which the drive path
// tolerates.
static uint32_t IO_bridge_private_dutyToCompare(float32_t duty, uint32_t period)
{
    return (uint32_t)((duty * (float32_t)period) + 0.5f);
}

// Convert one sense front end's latest ADC volts to amps. An unconfigured
// sense (voltsPerAmp == 0) or an ADC read failure returns false, leaving
// *amps_out untouched — so a missing/failed reading can never masquerade as a
// zero current to the overcurrent monitor.
static bool IO_bridge_private_readCurrent(
    const IO_bridge_currentSenseConfig_S * const sense, float32_t * const amps_out)
{
    bool ret = false;
    float32_t volts = 0.0f;
    if ((sense->voltsPerAmp != 0.0f) &&
        (HW_ADC_getVolts(sense->adcChannel, sense->adcInput, &volts)))
    {
        *amps_out = (volts - sense->zeroCurrentBias_V) / sense->voltsPerAmp;
        ret = true;
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
        for (size_t ch = 0U; (ch < config->numChannels) && success; ch++)
        {
            const IO_bridge_channelConfig_S * const channelConfig = &config->channels[ch];
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
                data->moePeripheral[ch] = periphU;

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
        ret = HW_TIM_setMainOutputEnabled(data->moePeripheral[channel], enabled);
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
        ret = HW_TIM_getMainOutputEnabled(data->moePeripheral[channel], enabled);
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
        ret = HW_TIM_clearBreakFlags(data->moePeripheral[channel]);
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
