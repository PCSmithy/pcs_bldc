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
    uint32_t counter;
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
static uint64_t HW_TIM_private_landings(uint64_t from, uint64_t counts, uint64_t span, uint64_t target);
static void HW_TIM_private_emitTrgo(HW_TIM_peripheral_E peripheral, uint64_t from, uint64_t counts);

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
// output port. Null-safe: an unnamed channel's handle stays invalid and the
// write no-ops. Raw counts never cross the boundary.
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

// How many times an up-counter advancing `counts` ticks from `from` takes the
// value `target`, modulo `span`. A counter already sitting on the target takes
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

// Emit a peripheral's trigger events for one advance of `counts` ticks from
// counter value `from`: each landing on the configured source's counter value —
// the reload value for an update source, the compare value for an OC-match one —
// is one trigger. A down-counter is handled by mirroring both positions about
// the span, so one landing count serves both directions.
static void HW_TIM_private_emitTrgo(HW_TIM_peripheral_E peripheral, uint64_t from, uint64_t counts)
{
    const HW_TIM_peripheralConfig_S * const peripheralConfig = &data->config->peripherals[peripheral];
    const HW_TIM_trgoSink_S * const sink = &data->trgoSink[peripheral];
    if ((peripheralConfig->configureTrgo) && (sink->callback != NULL))
    {
        const uint64_t span = (uint64_t)peripheralConfig->period + 1U;
        const bool     down = (peripheralConfig->countDir == HW_TIM_COUNT_DOWN);

        uint64_t target = span;   // no source resolved: out of range, emits nothing
        if (peripheralConfig->trgoSource == HW_TIM_TRGO_UPDATE)
        {
            target = down ? (uint64_t)peripheralConfig->period : 0U;
        }
        else if ((peripheralConfig->trgoSource == HW_TIM_TRGO_OC_MATCH) &&
                 (peripheralConfig->trgoOcUnit < HW_TIM_OC_UNITS_PER_PERIPHERAL))
        {
            target = data->peripheralData[peripheral].compare[peripheralConfig->trgoOcUnit];
        }
        else
        {
            // HW_TIM_TRGO_NONE, or an OC unit outside the peripheral's units.
        }

        if (target < span)
        {
            const uint64_t fromPos   = down ? ((span - from) % span) : from;
            const uint64_t targetPos = down ? ((span - target) % span) : target;
            const uint64_t landings  = HW_TIM_private_landings(fromPos, counts, span, targetPos);
            for (uint64_t n = 0U; n < landings; n++)
            {
                sink->callback(peripheral, sink->context);
            }
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
                data->peripheralData[p].counter =
                    (peripheralConfig->countDir == HW_TIM_COUNT_DOWN) ? peripheralConfig->period : 0U;

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
                const uint64_t span   = (uint64_t)peripheralConfig->period + 1U;
                const uint64_t counts = (uint64_t)elapsed_us * peripheralConfig->countsPerUs;
                const uint64_t cur    = data->peripheralData[p].counter;
                const uint64_t next   = (peripheralConfig->countDir == HW_TIM_COUNT_DOWN)
                    ? ((cur + span) - (counts % span)) % span
                    : (cur + counts) % span;
                data->peripheralData[p].counter = (uint32_t)next;
                HW_TIM_private_emitTrgo((HW_TIM_peripheral_E)p, cur, counts);
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
        *out = data->peripheralData[peripheral].counter;
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
