/* Includes */
#include "lib_types.h"
#include "IO_bridge.h"
#include "HW_TIM.h"

#include "IO_bridge_channels.h"

#include <array>

/* Defines */

/* Typedefs */

class Bridge
{
    public:
        bool init(IO_bridge_channel_E channel, const IO_bridge_channelConfig_S * const channelConfig);
        bool setPhaseDuty(IO_bridge_phase_E phase, float32_t duty01);
        bool setPhaseOutputEnabled(IO_bridge_phase_E phase, bool enabled);
        bool setOutputEnabled(bool enabled);
        bool getOutputEnabled(bool * const enabled);
        bool clearBreakFlags();

        bool getPhaseCurrent(IO_bridge_phase_E phase, float32_t * amps_out);
        bool getBusCurrent(float32_t * amps_out);

        bool registerCycleCallback(IO_bridge_cycleCallback_F callback,
                                     void * context);

        static void onInjectedComplete(HW_ADC_channels_E adcChannel, HW_ADC_conversionStatus_E status, void * context)
        {
            static_cast<Bridge *>(context)->recordInjectedSample(adcChannel, status);
        }

    private:
        const IO_bridge_channelConfig_S * config;
        IO_bridge_channel_E channelIndex;
        HW_TIM_peripheral_E moePeripheral;

        float32_t current_amps[IO_BRIDGE_PHASE_COUNT];
        uint32_t  sampleTime_us[IO_BRIDGE_PHASE_COUNT];
        uint32_t  updateCount[IO_BRIDGE_PHASE_COUNT];

        // Published by a task, read by the injected-completion ISR.
        IO_bridge_cycleCallback_F volatile cycleCallback;
        void * volatile                    cycleContext;

        void recordInjectedSample(HW_ADC_channels_E adcChannel, HW_ADC_conversionStatus_E status);
        void completeInjectedPair(uint32_t now);
};


typedef struct
{
    const IO_bridge_config_S * config;
    std::array<Bridge, IO_BRIDGE_CHANNEL_COUNT> channels;
} IO_bridge_data_S;

/* Private Free Function Declarations */

static uint32_t IO_bridge_private_dutyToCompare(float32_t duty, uint32_t period);
static bool IO_bridge_private_decodeCurrent(const IO_bridge_currentSenseConfig_S * const sense, float32_t volts, float32_t * const amps_out);
static bool IO_bridge_private_readInjectedCurrent(const IO_bridge_currentSenseConfig_S * const sense, float32_t * const amps_out);
static bool IO_bridge_private_registerInjected(const IO_bridge_config_S * const config);

/* Private Data Definitions */

static constinit IO_bridge_data_S IO_bridge_data = {};
static constinit IO_bridge_data_S * const data = &IO_bridge_data;

/* Private Free Function Definitions */

// Round duty x period to the nearest count.
static uint32_t IO_bridge_private_dutyToCompare(float32_t duty, uint32_t period)
{
    return (uint32_t)((duty * (float32_t)period) + 0.5f);
}

// value = (V_pin - V_bias) / scale
// [impl->fw~io_bridge_005~1]
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

// Hang each bridge's completion callback on every ADC channel carrying one of
// its injected phase senses, once per channel however many phases share it.
// The callback context is the bridge, so an ADC channel can belong to one
// bridge only: a second claimant is a configuration error.
static bool IO_bridge_private_registerInjected(const IO_bridge_config_S * const config)
{
    bool ret = true;
    size_t owner[HW_ADC_CHANNEL_COUNT];
    for (size_t adc = 0U; adc < HW_ADC_CHANNEL_COUNT; adc++)
    {
        owner[adc] = IO_BRIDGE_CHANNEL_COUNT;   // unclaimed
    }

    for (size_t channel = 0U; (channel < config->numChannels) && ret; channel++)
    {
        const IO_bridge_channelConfig_S * const channelConfig = &config->channels[channel];

        for (uint8_t phase = 0U; (phase < IO_BRIDGE_PHASE_COUNT) && ret; phase++)
        {
            const IO_bridge_currentSenseConfig_S * const sense = &channelConfig->phase[phase].currentSense;

            if (sense->injectedIndex != IO_BRIDGE_INJECTED_NONE)
            {
                if ((sense->adcChannel < HW_ADC_CHANNEL_COUNT) &&
                    (sense->injectedIndex < HW_ADC_INJECTED_INPUTS_PER_CHANNEL))
                {
                    if (owner[sense->adcChannel] == IO_BRIDGE_CHANNEL_COUNT)
                    {
                        ret = HW_ADC_registerInjectedCallback(sense->adcChannel,
                                                                &Bridge::onInjectedComplete,
                                                                &data->channels[channel]);
                        owner[sense->adcChannel] = channel;
                    }
                    else if (owner[sense->adcChannel] != channel)
                    {
                        ret = false;   // claimed by another bridge
                    }
                    else
                    {
                        // Another phase of the same bridge: already registered.
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

/* Private Class Function Definitions */

// U and V are sampled simultaneously - derive W from them by KCL
// [impl->fw~io_bridge_006~1]
void Bridge::completeInjectedPair(uint32_t now_us)
{
    this->current_amps[IO_BRIDGE_PHASE_W] = -(this->current_amps[IO_BRIDGE_PHASE_U] +
                                                this->current_amps[IO_BRIDGE_PHASE_V]);
    this->updateCount[IO_BRIDGE_PHASE_W] += 1U;
    this->sampleTime_us[IO_BRIDGE_PHASE_W] = now_us;

    // Last, so the whole triple is readable to the callback. One load each, so
    // a concurrent deregistration cannot null the pointer between test and call.
    // [impl->fw~io_bridge_007~1]
    const IO_bridge_cycleCallback_F callback = this->cycleCallback;
    void * const callbackContext = this->cycleContext;
    if (callback != NULL)
    {
        callback(this->channelIndex, callbackContext);
    }
}

void Bridge::recordInjectedSample(HW_ADC_channels_E adcChannel, HW_ADC_conversionStatus_E status)
{
    // Without a time base the pair window is meaningless: every stamp would
    // read zero and every sample would look simultaneous.
    uint32_t now_us;
    if ((this->config != NULL) &&
        (HW_TIM_getCounter(this->config->timeBasePeripheral, &now_us)) &&
        (status == HW_ADC_CONVERSION_STATUS_OK))
    {
        // lookup which phase measurement this ADC channel maps to
        for (uint8_t phase = 0U; phase < IO_BRIDGE_PHASE_COUNT; phase++) // TODO - memoize this / derive it statically at as a LUT at init
        {
            const IO_bridge_currentSenseConfig_S * const sense = &this->config->phase[phase].currentSense;
            if ((sense->adcChannel == adcChannel) &&
                (sense->injectedIndex != IO_BRIDGE_INJECTED_NONE))
            {
                float32_t amps = 0.0f;
                if (IO_bridge_private_readInjectedCurrent(sense, &amps))
                {
                    this->current_amps[phase] = amps;
                    this->updateCount[phase] += 1U;
                    this->sampleTime_us[phase] = now_us;

                    // [impl->fw~io_bridge_006~1]
                    // check if the complement phase has already been updated - if so
                    // we can derive the 3rd phase current via KCL
                    const IO_bridge_phase_E partner = this->config->phase[phase].complementPhase;
                    if (partner < IO_BRIDGE_PHASE_COUNT) // the derived phase has no partner
                    {
                        // Unsigned subtract is wrap-safe; the partner's stamp is always in the past.
                        const uint32_t timeSincePartner_us = now_us - this->sampleTime_us[partner];
                        if ((this->updateCount[partner] != 0U) &&
                            (timeSincePartner_us <= this->config->injectedPairWindow_us))
                        {
                            this->completeInjectedPair(now_us);
                        }
                    }
                }
            }
        }
    }
}

/* Public Class Function Definitions */

// [impl->fw~io_bridge_001~1] one bridge's share of init: its three phases must
// resolve to one HW_TIM peripheral (that peripheral's MOE is the whole-bridge
// gate) and its complement table must be symmetric; then each phase's
// output-compare unit is enabled. Outputs stay dark because HW_TIM commands
// MOE off at init, leaving the master output enable as the sole runtime gate.
bool Bridge::init(IO_bridge_channel_E channel, const IO_bridge_channelConfig_S * const channelConfig)
{
    bool ret = false;
    if (channelConfig != NULL)
    {
        HW_TIM_peripheral_E periph[IO_BRIDGE_PHASE_COUNT] = {};
        bool phasesOk = true;
        for (uint8_t p = 0U; (p < IO_BRIDGE_PHASE_COUNT) && phasesOk; p++)
        {
            const IO_bridge_phaseConfig_S * const phase = &channelConfig->phase[p];
            // getPeripheral fails on an out-of-range phase channel.
            phasesOk = HW_TIM_getPeripheral(phase->tim, &periph[p]);
            // A complement is none, or a different phase whose complement is this one.
            const IO_bridge_phase_E c = phase->complementPhase;
            if (c != IO_BRIDGE_PHASE_COUNT)
            {
                phasesOk = phasesOk &&
                           (c < IO_BRIDGE_PHASE_COUNT) &&
                           (c != p) &&
                           (channelConfig->phase[c].complementPhase == p);
            }
        }
        if (phasesOk &&
            (periph[IO_BRIDGE_PHASE_U] == periph[IO_BRIDGE_PHASE_V]) &&
            (periph[IO_BRIDGE_PHASE_V] == periph[IO_BRIDGE_PHASE_W]))
        {
            this->channelIndex  = channel;
            this->config        = channelConfig;
            this->moePeripheral = periph[IO_BRIDGE_PHASE_U];
            ret = (HW_TIM_setOutputEnabled(channelConfig->phase[IO_BRIDGE_PHASE_U].tim, true) &&
                   HW_TIM_setOutputEnabled(channelConfig->phase[IO_BRIDGE_PHASE_V].tim, true) &&
                   HW_TIM_setOutputEnabled(channelConfig->phase[IO_BRIDGE_PHASE_W].tim, true));
        }
    }
    return ret;
}

// Every public member checks its own arguments and that the bridge is
// initialized (config set by init), so each is safe from any caller; the C
// facade adds only the channel index it uses to reach the bridge.
bool Bridge::setPhaseDuty(IO_bridge_phase_E phase, float32_t duty01)
{
    bool ret = false;
    if ((this->config != NULL) &&
        (phase < IO_BRIDGE_PHASE_COUNT) &&
        (duty01 >= 0.0f) &&
        (duty01 <= 1.0f))
    {
        const HW_TIM_channels_E timChannel = this->config->phase[phase].tim;
        uint32_t period = 0U;
        if (HW_TIM_getPeriod(timChannel, &period))
        {
            const uint32_t compare = IO_bridge_private_dutyToCompare(duty01, period);
            ret = HW_TIM_setCompare(timChannel, compare);
        }
    }
    return ret;
}

bool Bridge::setPhaseOutputEnabled(IO_bridge_phase_E phase, bool enabled)
{
    return (this->config != NULL) &&
           (phase < IO_BRIDGE_PHASE_COUNT) &&
           HW_TIM_setOutputEnabled(this->config->phase[phase].tim, enabled);
}

bool Bridge::setOutputEnabled(bool enabled)
{
    return (this->config != NULL) && HW_TIM_setMainOutputEnabled(this->moePeripheral, enabled);
}

bool Bridge::getOutputEnabled(bool * const enabled)
{
    return (this->config != NULL) && (enabled != NULL) &&
           HW_TIM_getMainOutputEnabled(this->moePeripheral, enabled);
}

bool Bridge::clearBreakFlags()
{
    return (this->config != NULL) && HW_TIM_clearBreakFlags(this->moePeripheral);
}

// [impl->fw~io_bridge_005~1] the most recent injected (PWM-crest) sample, never
// the regular-sequence one; zero before the first.
bool Bridge::getPhaseCurrent(IO_bridge_phase_E phase, float32_t * amps_out)
{
    bool ret = false;
    if ((this->config != NULL) &&
        (phase < IO_BRIDGE_PHASE_COUNT) &&
        (amps_out != NULL))
    {
        *amps_out = this->current_amps[phase];
        ret = true;
    }
    return ret;
}

// [impl->fw~io_bridge_005~1]
bool Bridge::getBusCurrent(float32_t * amps_out)
{
    bool ret = false;
    if ((this->config != NULL) && (amps_out != NULL))
    {
        float32_t volts = 0.0f;
        const IO_bridge_currentSenseConfig_S * const sense = &this->config->busCurrent;
        if (HW_ADC_getVolts(sense->adcChannel, sense->adcInput, &volts))
        {
            ret = IO_bridge_private_decodeCurrent(sense, volts, amps_out);
        }
    }
    return ret;
}

bool Bridge::registerCycleCallback(IO_bridge_cycleCallback_F callback, void * context)
{
    bool ret = false;
    if ((this->config != NULL) && (callback != NULL))
    {
        // Context first: the ISR can fire between the stores, and must never
        // pair a newly published callback with the previous context.
        this->cycleContext  = context;
        this->cycleCallback = callback;
        ret = true;
    }
    return ret;
}

/* Public Function Definitions */

// The C facade owns one check, the index it uses to reach the bridge; every
// other precondition lives in the member it forwards to.

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
            success = data->channels[channel].init(static_cast<IO_bridge_channel_E>(channel),
                                                   &config->channels[channel]);
        }
        if (success)
        {
            // Bridges are configured before their callbacks exist: HW_ADC_init
            // armed the injected group before this runs, so the first
            // completion can land the moment a callback is registered.
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
    return (channel < IO_BRIDGE_CHANNEL_COUNT) && data->channels[channel].setPhaseDuty(phase, duty01);
}

// [impl->fw~io_bridge_004~1]
bool IO_bridge_setPhaseOutputEnabled(IO_bridge_channel_E channel, IO_bridge_phase_E phase, bool enabled)
{
    return (channel < IO_BRIDGE_CHANNEL_COUNT) && data->channels[channel].setPhaseOutputEnabled(phase, enabled);
}

// [impl->fw~io_bridge_003~1]
bool IO_bridge_setOutputEnabled(IO_bridge_channel_E channel, bool enabled)
{
    return (channel < IO_BRIDGE_CHANNEL_COUNT) && data->channels[channel].setOutputEnabled(enabled);
}

// [impl->fw~io_bridge_003~1]
bool IO_bridge_getOutputEnabled(IO_bridge_channel_E channel, bool * const enabled)
{
    return (channel < IO_BRIDGE_CHANNEL_COUNT) && data->channels[channel].getOutputEnabled(enabled);
}

bool IO_bridge_clearBreakFlags(IO_bridge_channel_E channel)
{
    return (channel < IO_BRIDGE_CHANNEL_COUNT) && data->channels[channel].clearBreakFlags();
}

// [impl->fw~io_bridge_005~1]
bool IO_bridge_getPhaseCurrent(IO_bridge_channel_E channel, IO_bridge_phase_E phase, float32_t * const amps_out)
{
    return (channel < IO_BRIDGE_CHANNEL_COUNT) && data->channels[channel].getPhaseCurrent(phase, amps_out);
}

// [impl->fw~io_bridge_005~1]
bool IO_bridge_getBusCurrent(IO_bridge_channel_E channel, float32_t * const amps_out)
{
    return (channel < IO_BRIDGE_CHANNEL_COUNT) && data->channels[channel].getBusCurrent(amps_out);
}

// [impl->fw~io_bridge_007~1]
bool IO_bridge_registerCycleCallback(IO_bridge_channel_E channel,
                                     IO_bridge_cycleCallback_F callback,
                                     void * context)
{
    return (channel < IO_BRIDGE_CHANNEL_COUNT) && data->channels[channel].registerCycleCallback(callback, context);
}

