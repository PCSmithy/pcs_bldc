/* Includes */

#include <stdio.h>

#include "lib_types.h"

#include "HW_TIM.h"
#include "SIL_ports.h"

/* Defines */

// Largest SIL port id segment the driver formats (<channelNameStr>_enabled).
#define HW_TIM_SIM_PORT_NAME_MAX  (40U)

/* Typedefs */

typedef struct
{
    // Position within one counting cycle; the counter value follows from it and
    // the direction. A center-aligned counter reads the same value twice a cycle.
    uint64_t position;
    // Reload events still to come before the next update event.
    uint32_t repetitionRemaining;
    uint32_t compare[HW_TIM_OC_UNITS_PER_PERIPHERAL];
    bool     outputEnabled[HW_TIM_OC_UNITS_PER_PERIPHERAL];
    // MOE latch gating every enabled output. Commanded OFF at init; a break
    // clears it via a table write to this static and it stays clear until
    // HW_TIM_setMainOutputEnabled sets it again.
    bool     mainOutputEnabled;
} HW_TIM_peripheralData_S;

typedef struct
{
    HW_TIM_trgoCallback_F callback;
    void *                context;
} HW_TIM_trgoSink_S;

typedef struct
{
    const HW_TIM_config_S * config;
    HW_TIM_peripheralData_S peripheralData[HW_TIM_PERIPHERAL_COUNT];

    // Trigger-output sinks, one per peripheral. HW_TIM_init leaves them alone
    // so registration order against init does not matter.
    HW_TIM_trgoSink_S trgoSink[HW_TIM_PERIPHERAL_COUNT];

    // SIL output-port handles (SIL_PORTS_HANDLE_INVALID when unregistered): the
    // commanded bridge state a motor model consumes — normalized per-phase duty
    // + per-phase enable per logical channel, one master output enable per
    // advanced-control peripheral. Publication is event-driven from the setters.
    int32_t dutyHandle[HW_TIM_CHANNEL_COUNT];
    int32_t enabledHandle[HW_TIM_CHANNEL_COUNT];
    int32_t moeHandle[HW_TIM_PERIPHERAL_COUNT];

    bool initialized;
} HW_TIM_data_S;

/* Private Data Definitions */

static HW_TIM_data_S HW_TIM_data;
static HW_TIM_data_S * const data = &HW_TIM_data;

/* Private Function Declarations */

static uint32_t HW_TIM_private_maxForWidth(uint32_t widthBits);
static bool HW_TIM_private_validatePeripheral(const HW_TIM_peripheralConfig_S * const peripheralConfig);
static bool HW_TIM_private_validateChannel(const HW_TIM_config_S * const config,
                                           const HW_TIM_channelConfig_S * const channelConfig);
static void HW_TIM_private_publishDuty(HW_TIM_channels_E channel);
static void HW_TIM_private_publishEnabled(HW_TIM_channels_E channel);
static void HW_TIM_private_publishMoe(HW_TIM_peripheral_E peripheral);
static uint64_t HW_TIM_private_cycleCounts(const HW_TIM_peripheralConfig_S * const peripheralConfig);
static uint32_t HW_TIM_private_counterAt(const HW_TIM_peripheralConfig_S * const peripheralConfig,
                                         uint64_t position);
static uint64_t HW_TIM_private_landings(uint64_t from, uint64_t counts, uint64_t span, uint64_t target);
static uint64_t HW_TIM_private_reloadEvents(const HW_TIM_peripheralConfig_S * const peripheralConfig,
                                            uint64_t from, uint64_t counts);
static uint64_t HW_TIM_private_consumeRepetition(HW_TIM_peripheral_E peripheral, uint64_t reloads);
static void HW_TIM_private_emitTrgo(HW_TIM_peripheral_E peripheral, uint64_t from, uint64_t counts,
                                    uint64_t updates);

/* Private Function Definitions */

static uint32_t HW_TIM_private_maxForWidth(uint32_t widthBits)
{
    uint32_t max = 0xFFFFFFFFUL;
    if (widthBits < 32U)
    {
        max = (1UL << widthBits) - 1UL;
    }
    return max;
}

static bool HW_TIM_private_validatePeripheral(const HW_TIM_peripheralConfig_S * const peripheralConfig)
{
    bool valid = ((peripheralConfig->countDir == HW_TIM_COUNT_UP)   ||
                  (peripheralConfig->countDir == HW_TIM_COUNT_DOWN) ||
                  (peripheralConfig->countDir == HW_TIM_COUNT_CENTER));

    if (valid && (peripheralConfig->period > HW_TIM_private_maxForWidth(peripheralConfig->counterWidthBits)))
    {
        valid = false;
    }

    if (valid && peripheralConfig->configureBreakDeadTime && (peripheralConfig->deadTime > 0xFFU))
    {
        valid = false;
    }

    return valid;
}

static bool HW_TIM_private_validateChannel(const HW_TIM_config_S * const config,
                                           const HW_TIM_channelConfig_S * const channelConfig)
{
    bool valid = ((channelConfig->role == HW_TIM_ROLE_OUTPUT_COMPARE)          &&
                  ((size_t)channelConfig->peripheral < config->numPeripherals) &&
                  (channelConfig->ocUnit < HW_TIM_OC_UNITS_PER_PERIPHERAL));

    if (valid && (channelConfig->compare > config->peripherals[channelConfig->peripheral].period))
    {
        valid = false;
    }

    return valid;
}

// Publish a logical channel's normalized duty (compare / period ∈ [0,1]) on its
// output port. Raw counts never cross the boundary.
static void HW_TIM_private_publishDuty(HW_TIM_channels_E channel)
{
    const HW_TIM_channelConfig_S * const channelConfig = &data->config->channels[channel];
    const HW_TIM_peripheral_E peripheral = channelConfig->peripheral;
    const uint32_t period  = data->config->peripherals[peripheral].period;
    const uint32_t compare = data->peripheralData[peripheral].compare[channelConfig->ocUnit];
    const double duty = (period > 0U) ? ((double)compare / (double)period) : 0.0;
    SIL_ports_write(data->dutyHandle[channel], duty);
}

// Publish a logical channel's output enable as 0.0/1.0.
static void HW_TIM_private_publishEnabled(HW_TIM_channels_E channel)
{
    const HW_TIM_channelConfig_S * const channelConfig = &data->config->channels[channel];
    const bool enabled = data->peripheralData[channelConfig->peripheral].outputEnabled[channelConfig->ocUnit];
    SIL_ports_write(data->enabledHandle[channel], enabled ? 1.0 : 0.0);
}

// Publish a peripheral's master output enable as 0.0/1.0.
static void HW_TIM_private_publishMoe(HW_TIM_peripheral_E peripheral)
{
    const bool enabled = data->peripheralData[peripheral].mainOutputEnabled;
    SIL_ports_write(data->moeHandle[peripheral], enabled ? 1.0 : 0.0);
}

// Counter positions in one counting cycle: an up- or down-counter walks
// period + 1 of them, a center-aligned counter 2 * period. Zero period, one
// position.
static uint64_t HW_TIM_private_cycleCounts(const HW_TIM_peripheralConfig_S * const peripheralConfig)
{
    const uint64_t period = peripheralConfig->period;
    return ((peripheralConfig->countDir == HW_TIM_COUNT_CENTER) && (period > 0U))
        ? (2U * period)
        : (period + 1U);
}

// Counter value at a cycle position: an up-counter reads the position straight,
// a down-counter its mirror about the period, a center-aligned one either by phase.
static uint32_t HW_TIM_private_counterAt(const HW_TIM_peripheralConfig_S * const peripheralConfig,
                                         uint64_t position)
{
    const uint64_t period = peripheralConfig->period;
    uint64_t counter = position;
    if (peripheralConfig->countDir == HW_TIM_COUNT_DOWN)
    {
        counter = period - position;
    }
    else if ((peripheralConfig->countDir == HW_TIM_COUNT_CENTER) && (position > period))
    {
        counter = (2U * period) - position;
    }
    else
    {
        // Up-counting, or the up phase of a center-aligned cycle.
    }
    return (uint32_t)counter;
}

// How many times a position advancing `counts` ticks from `from` takes the
// value `target`, modulo `span`. A position already sitting on the target takes
// it again a full lap later, so an advance landing exactly on the target counts
// once whichever side of it the advance started.
static uint64_t HW_TIM_private_landings(uint64_t from, uint64_t counts, uint64_t span, uint64_t target)
{
    uint64_t landings = 0U;
    uint64_t first = ((span + target) - from) % span;   // ticks to the first landing
    if (first == 0U)
    {
        first = span;
    }
    if (counts >= first)
    {
        landings = ((counts - first) / span) + 1U;
    }
    return landings;
}

// Reload events one advance of `counts` positions from `from` crosses. Position
// zero reloads every counter; a center-aligned one also reloads at the period.
static uint64_t HW_TIM_private_reloadEvents(const HW_TIM_peripheralConfig_S * const peripheralConfig,
                                            uint64_t from, uint64_t counts)
{
    const uint64_t cycle = HW_TIM_private_cycleCounts(peripheralConfig);
    uint64_t reloads = HW_TIM_private_landings(from, counts, cycle, 0U);
    if ((peripheralConfig->countDir == HW_TIM_COUNT_CENTER) && (peripheralConfig->period > 0U))
    {
        reloads += HW_TIM_private_landings(from, counts, cycle, peripheralConfig->period);
    }
    return reloads;
}

// Run `reloads` reload events through a peripheral's repetition countdown and
// report the update events they produce: one every rcr + 1 reloads. The
// countdown starts at rcr, so with rcr = 1 on a center-aligned counter started
// at zero the crest spends the countdown and the valley expires it.
static uint64_t HW_TIM_private_consumeRepetition(HW_TIM_peripheral_E peripheral, uint64_t reloads)
{
    const uint64_t span = (uint64_t)data->config->peripherals[peripheral].rcr + 1U;
    const uint64_t remaining = data->peripheralData[peripheral].repetitionRemaining;

    uint64_t updates = 0U;
    if (reloads > remaining)
    {
        updates = (((reloads - remaining) - 1U) / span) + 1U;
    }
    data->peripheralData[peripheral].repetitionRemaining =
        (uint32_t)(((remaining + span) - (reloads % span)) % span);
    return updates;
}

// Emit a peripheral's trigger events for one advance of `counts` positions from
// `from`, which produced `updates` update events: an update source emits one per
// update event, an OC-match source one per landing on the compare value.
static void HW_TIM_private_emitTrgo(HW_TIM_peripheral_E peripheral, uint64_t from, uint64_t counts,
                                    uint64_t updates)
{
    const HW_TIM_peripheralConfig_S * const peripheralConfig = &data->config->peripherals[peripheral];
    const HW_TIM_trgoSink_S * const sink = &data->trgoSink[peripheral];
    if ((peripheralConfig->configureTrgo) && (sink->callback != NULL))
    {
        const uint64_t period = peripheralConfig->period;
        const uint64_t cycle  = HW_TIM_private_cycleCounts(peripheralConfig);

        uint64_t upTriggers   = 0U;
        uint64_t downTriggers = 0U;
        if (peripheralConfig->trgoSource == HW_TIM_TRGO_UPDATE)
        {
            // TRGO-on-update is a pulse on silicon: the consumer's edge select
            // sees a rising edge whichever way the counter was going.
            upTriggers = updates;
        }
        else if ((peripheralConfig->trgoSource == HW_TIM_TRGO_OC_MATCH) &&
                 (peripheralConfig->trgoOcUnit < HW_TIM_OC_UNITS_PER_PERIPHERAL))
        {
            // No CCR shadow modelled: a compare written between advances applies
            // to the whole of the next one.
            const uint64_t compare = data->peripheralData[peripheral].compare[peripheralConfig->trgoOcUnit];
            const uint64_t match = (peripheralConfig->countDir == HW_TIM_COUNT_DOWN)
                ? (period - compare)
                : compare;
            if (peripheralConfig->countDir == HW_TIM_COUNT_DOWN)
            {
                downTriggers = HW_TIM_private_landings(from, counts, cycle, match);
            }
            else
            {
                upTriggers = HW_TIM_private_landings(from, counts, cycle, match);
            }

            if (peripheralConfig->countDir == HW_TIM_COUNT_CENTER)
            {
                // The down phase takes the same compare value at the mirrored
                // position, except at either extreme where the two coincide.
                const uint64_t mirror = ((2U * period) - compare) % cycle;
                if (mirror != match)
                {
                    downTriggers = HW_TIM_private_landings(from, counts, cycle, mirror);
                }
                else if (compare == 0U)
                {
                    // The extremes collapse to one landing; the counter arrives
                    // at the valley falling, at the crest rising.
                    downTriggers = upTriggers;
                    upTriggers   = 0U;
                }
                else
                {
                    // Crest: one landing, arrived at counting up.
                }
            }
        }
        else
        {
            // HW_TIM_TRGO_NONE, or an OC unit outside the peripheral's units.
        }

        // Batched per advance (up-phase landings, then down-phase). Ordering
        // within one advance is undefined; a sink must not depend on it.
        for (uint64_t n = 0U; n < upTriggers; n++)
        {
            sink->callback(peripheral, HW_TIM_TRGO_CROSS_UP, sink->context);
        }
        for (uint64_t n = 0U; n < downTriggers; n++)
        {
            sink->callback(peripheral, HW_TIM_TRGO_CROSS_DOWN, sink->context);
        }
    }
}

/* Public Function Definitions */

// [impl->fw~hal_tim_001~1]
// [impl->fw~hal_tim_005~1]
// [impl->fw~hal_tim_007~1]
bool HW_TIM_init(const HW_TIM_config_S * const config)
{
    bool ret = false;
    if ((config != NULL) &&
        (config->peripherals != NULL) &&
        (config->channels != NULL) &&
        (config->numPeripherals <= HW_TIM_PERIPHERAL_COUNT) &&
        (config->numChannels <= HW_TIM_CHANNEL_COUNT))
    {
        bool valid = true;
        for (size_t p = 0U; (p < config->numPeripherals) && valid; p++)
        {
            valid = HW_TIM_private_validatePeripheral(&config->peripherals[p]);
        }
        for (size_t ch = 0U; (ch < config->numChannels) && valid; ch++)
        {
            valid = HW_TIM_private_validateChannel(config, &config->channels[ch]);
        }

        if (valid)
        {
            data->config = config;
            for (size_t p = 0U; p < HW_TIM_PERIPHERAL_COUNT; p++)
            {
                data->peripheralData[p] = (HW_TIM_peripheralData_S){ 0 };
                data->moeHandle[p] = SIL_PORTS_HANDLE_INVALID;
            }
            for (size_t ch = 0U; ch < HW_TIM_CHANNEL_COUNT; ch++)
            {
                data->dutyHandle[ch]    = SIL_PORTS_HANDLE_INVALID;
                data->enabledHandle[ch] = SIL_PORTS_HANDLE_INVALID;
            }

            for (size_t p = 0U; p < config->numPeripherals; p++)
            {
                const HW_TIM_peripheralConfig_S * const peripheralConfig = &config->peripherals[p];
                // Every direction starts its cycle at position zero — the
                // counter reads zero, or the period on a down-counter — with a
                // full repetition countdown ahead of the first update event.
                data->peripheralData[p].position = 0U;
                data->peripheralData[p].repetitionRemaining = peripheralConfig->rcr;

                // The advanced-control peripheral (the one with a break input,
                // hence an MOE latch) publishes a master-output-enable port,
                // named from its peripheral name string.
                if (peripheralConfig->hasBreakInput && (peripheralConfig->nameStr != NULL))
                {
                    char name[HW_TIM_SIM_PORT_NAME_MAX];
                    (void)snprintf(name, sizeof(name), "%s_MOE", peripheralConfig->nameStr);
                    data->moeHandle[p] = SIL_ports_register("vsig", name, NULL);
                }
            }

            // Seed each logical channel's initial compare and register its duty +
            // enable ports (native format: normalized duty, 0/1 enable).
            for (size_t ch = 0U; ch < config->numChannels; ch++)
            {
                const HW_TIM_channelConfig_S * const channelConfig = &config->channels[ch];
                data->peripheralData[channelConfig->peripheral].compare[channelConfig->ocUnit] =
                    channelConfig->compare;

                if (channelConfig->channelNameStr != NULL)
                {
                    char name[HW_TIM_SIM_PORT_NAME_MAX];
                    (void)snprintf(name, sizeof(name), "%s_duty", channelConfig->channelNameStr);
                    data->dutyHandle[ch] = SIL_ports_register("vsig", name, NULL);
                    (void)snprintf(name, sizeof(name), "%s_enabled", channelConfig->channelNameStr);
                    data->enabledHandle[ch] = SIL_ports_register("vsig", name, NULL);
                }
            }
            data->initialized = true;

            // Publish the boot state (bridge dark: duty 0, enables off, MOE off).
            // The writes buffer in the framework until the first out-sync.
            for (size_t ch = 0U; ch < config->numChannels; ch++)
            {
                HW_TIM_private_publishDuty((HW_TIM_channels_E)ch);
                HW_TIM_private_publishEnabled((HW_TIM_channels_E)ch);
            }
            for (size_t p = 0U; p < config->numPeripherals; p++)
            {
                HW_TIM_private_publishMoe((HW_TIM_peripheral_E)p);
            }
            ret = true;
        }
    }
    return ret;
}

// [impl->fw~hal_tim_006~1]
void HW_TIM_advanceTime(uint32_t elapsed_us)
{
    if (data->initialized)
    {
        for (size_t p = 0U; p < data->config->numPeripherals; p++)
        {
            const HW_TIM_peripheralConfig_S * const peripheralConfig = &data->config->peripherals[p];
            if (peripheralConfig->countsPerUs > 0U)
            {
                const uint64_t cycle  = HW_TIM_private_cycleCounts(peripheralConfig);
                const uint64_t counts = (uint64_t)elapsed_us * peripheralConfig->countsPerUs;
                const uint64_t from   = data->peripheralData[p].position;
                data->peripheralData[p].position = (from + counts) % cycle;

                const uint64_t reloads = HW_TIM_private_reloadEvents(peripheralConfig, from, counts);
                const uint64_t updates = HW_TIM_private_consumeRepetition((HW_TIM_peripheral_E)p, reloads);
                HW_TIM_private_emitTrgo((HW_TIM_peripheral_E)p, from, counts, updates);
            }
        }
    }
}

// [impl->fw~hal_tim_006~1]
bool HW_TIM_registerTrgoCallback(HW_TIM_peripheral_E peripheral,
                                 HW_TIM_trgoCallback_F callback,
                                 void * context)
{
    bool ret = false;
    if (peripheral < HW_TIM_PERIPHERAL_COUNT)
    {
        data->trgoSink[peripheral].callback = callback;
        data->trgoSink[peripheral].context  = context;
        ret = true;
    }
    return ret;
}

// [impl->fw~hal_tim_003~1]
bool HW_TIM_getCounter(HW_TIM_peripheral_E peripheral, uint32_t * const out)
{
    bool ret = false;
    if ((out != NULL) &&
        (data->initialized) &&
        (peripheral < HW_TIM_PERIPHERAL_COUNT) &&
        ((size_t)peripheral < data->config->numPeripherals))
    {
        *out = HW_TIM_private_counterAt(&data->config->peripherals[peripheral],
                                        data->peripheralData[peripheral].position);
        ret = true;
    }
    return ret;
}

bool HW_TIM_getPeripheral(HW_TIM_channels_E channel, HW_TIM_peripheral_E * const out)
{
    bool ret = false;
    if ((out != NULL) &&
        (data->initialized) &&
        (channel < HW_TIM_CHANNEL_COUNT) &&
        ((size_t)channel < data->config->numChannels))
    {
        *out = data->config->channels[channel].peripheral;
        ret = true;
    }
    return ret;
}

bool HW_TIM_getPeriod(HW_TIM_channels_E channel, uint32_t * const out)
{
    bool ret = false;
    if ((out != NULL) &&
        (data->initialized) &&
        (channel < HW_TIM_CHANNEL_COUNT) &&
        ((size_t)channel < data->config->numChannels))
    {
        const HW_TIM_peripheral_E peripheral = data->config->channels[channel].peripheral;
        *out = data->config->peripherals[peripheral].period;
        ret = true;
    }
    return ret;
}

// [impl->fw~hal_tim_004~1]
bool HW_TIM_setCompare(HW_TIM_channels_E channel, uint32_t counts)
{
    bool ret = false;
    if ((data->initialized) &&
        (channel < HW_TIM_CHANNEL_COUNT) &&
        ((size_t)channel < data->config->numChannels))
    {
        const HW_TIM_channelConfig_S * const channelConfig = &data->config->channels[channel];
        const HW_TIM_peripheral_E peripheral = channelConfig->peripheral;
        if (counts <= data->config->peripherals[peripheral].period)
        {
            data->peripheralData[peripheral].compare[channelConfig->ocUnit] = counts;
            HW_TIM_private_publishDuty(channel);
            ret = true;
        }
    }
    return ret;
}

// [impl->fw~hal_tim_004~1]
bool HW_TIM_getCompare(HW_TIM_channels_E channel, uint32_t * const out)
{
    bool ret = false;
    if ((out != NULL) &&
        (data->initialized) &&
        (channel < HW_TIM_CHANNEL_COUNT) &&
        ((size_t)channel < data->config->numChannels))
    {
        const HW_TIM_channelConfig_S * const channelConfig = &data->config->channels[channel];
        *out = data->peripheralData[channelConfig->peripheral].compare[channelConfig->ocUnit];
        ret = true;
    }
    return ret;
}

// [impl->fw~hal_tim_004~1]
bool HW_TIM_setOutputEnabled(HW_TIM_channels_E channel, bool enabled)
{
    bool ret = false;
    if ((data->initialized) &&
        (channel < HW_TIM_CHANNEL_COUNT) &&
        ((size_t)channel < data->config->numChannels))
    {
        const HW_TIM_channelConfig_S * const channelConfig = &data->config->channels[channel];
        data->peripheralData[channelConfig->peripheral].outputEnabled[channelConfig->ocUnit] = enabled;
        HW_TIM_private_publishEnabled(channel);
        ret = true;
    }
    return ret;
}

// [impl->fw~hal_tim_008~1]
bool HW_TIM_setMainOutputEnabled(HW_TIM_peripheral_E peripheral, bool enabled)
{
    bool ret = false;
    if ((data->initialized) &&
        (peripheral < HW_TIM_PERIPHERAL_COUNT) &&
        ((size_t)peripheral < data->config->numPeripherals))
    {
        data->peripheralData[peripheral].mainOutputEnabled = enabled;
        HW_TIM_private_publishMoe(peripheral);
        ret = true;
    }
    return ret;
}

// [impl->fw~hal_tim_008~1]
bool HW_TIM_getMainOutputEnabled(HW_TIM_peripheral_E peripheral, bool * const enabled)
{
    bool ret = false;
    if ((enabled != NULL) &&
        (data->initialized) &&
        (peripheral < HW_TIM_PERIPHERAL_COUNT) &&
        ((size_t)peripheral < data->config->numPeripherals))
    {
        *enabled = data->peripheralData[peripheral].mainOutputEnabled;
        ret = true;
    }
    return ret;
}

bool HW_TIM_clearBreakFlags(HW_TIM_peripheral_E peripheral)
{
    bool ret = false;
    if ((data->initialized) &&
        (peripheral < HW_TIM_PERIPHERAL_COUNT) &&
        ((size_t)peripheral < data->config->numPeripherals))
    {
        // No latched break flags in the sim model; success means valid args.
        ret = true;
    }
    return ret;
}
