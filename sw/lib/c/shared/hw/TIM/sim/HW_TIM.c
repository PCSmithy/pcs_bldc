/* Includes */

#include "lib_types.h"

#include "HW_TIM.h"
#include "HW_TIM_sim.h"

/* Defines */

/* Typedefs */

typedef struct
{
    uint32_t counter;
    bool     centerGoingUp;                            // direction within a center-aligned ramp
    uint32_t compare[HW_TIM_OC_UNITS_PER_PERIPHERAL];
    bool     outputEnabled[HW_TIM_OC_UNITS_PER_PERIPHERAL];
    bool     ocConfigured[HW_TIM_OC_UNITS_PER_PERIPHERAL];  // a logical channel owns this unit
    // MOE latch gating every enabled output. Commanded OFF at init; a break
    // assertion clears it and it stays clear (no auto-restore) until set again.
    bool     mainOutputEnabled;
    uint32_t triggerCount;
} HW_TIM_peripheralData_S;

typedef struct
{
    const HW_TIM_config_S * config;
    HW_TIM_peripheralData_S peripheralData[HW_TIM_PERIPHERAL_COUNT];
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
static uint32_t HW_TIM_private_activeLevel(uint32_t inactiveLevel);

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

static uint32_t HW_TIM_private_activeLevel(uint32_t inactiveLevel)
{
    return (inactiveLevel != 0U) ? 0U : 1U;
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

/* Public Function Definitions */

// [impl->fw~hal_tim_001~1]
// [impl->fw~hal_tim_002~1]
// [impl->fw~hal_tim_005~1]
// [impl->fw~hal_tim_006~1]
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
            for (size_t p = 0U; p < config->numPeripherals; p++)
            {
                const HW_TIM_peripheralConfig_S * const peripheralConfig = &config->peripherals[p];
                HW_TIM_peripheralData_S * const peripheralData = &data->peripheralData[p];

                *peripheralData = (HW_TIM_peripheralData_S){ 0 };
                peripheralData->centerGoingUp = true;
                peripheralData->counter =
                    (peripheralConfig->countDir == HW_TIM_COUNT_DOWN) ? peripheralConfig->period : 0U;
            }

            // Seed each logical channel's initial compare into its peripheral's
            // per-unit slot and mark the unit configured.
            for (size_t ch = 0U; ch < config->numChannels; ch++)
            {
                const HW_TIM_channelConfig_S * const channelConfig = &config->channels[ch];
                HW_TIM_peripheralData_S * const peripheralData =
                    &data->peripheralData[channelConfig->peripheral];
                peripheralData->compare[channelConfig->ocUnit] = channelConfig->compare;
                peripheralData->ocConfigured[channelConfig->ocUnit] = true;
            }
            data->initialized = true;
            ret = true;
        }
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

/* SIL inspection / control API */

void HW_TIM_sim_reset(void)
{
    *data = (HW_TIM_data_S){ 0 };
}

// [impl->fw~hal_tim_002~1]
// [impl->fw~hal_tim_006~1]
void HW_TIM_sim_advance(HW_TIM_peripheral_E peripheral, uint32_t ticks)
{
    if ((data->initialized) &&
        (peripheral < HW_TIM_PERIPHERAL_COUNT) &&
        ((size_t)peripheral < data->config->numPeripherals))
    {
        const HW_TIM_peripheralConfig_S * const peripheralConfig = &data->config->peripherals[peripheral];
        HW_TIM_peripheralData_S * const peripheralData = &data->peripheralData[peripheral];
        const uint32_t period = peripheralConfig->period;

        for (uint32_t step = 0U; step < ticks; step++)
        {
            bool update = false;

            switch (peripheralConfig->countDir)
            {
                case HW_TIM_COUNT_UP:
                    if (peripheralData->counter >= period)
                    {
                        peripheralData->counter = 0U;
                        update = true;
                    }
                    else
                    {
                        peripheralData->counter++;
                    }
                    break;

                case HW_TIM_COUNT_DOWN:
                    if (peripheralData->counter == 0U)
                    {
                        peripheralData->counter = period;
                        update = true;
                    }
                    else
                    {
                        peripheralData->counter--;
                    }
                    break;

                case HW_TIM_COUNT_CENTER:
                default:
                    if (peripheralData->centerGoingUp)
                    {
                        peripheralData->counter++;
                        if (peripheralData->counter >= period)
                        {
                            peripheralData->counter = period;
                            peripheralData->centerGoingUp = false;
                        }
                    }
                    else if (peripheralData->counter == 0U)
                    {
                        peripheralData->centerGoingUp = true;
                        update = true;
                    }
                    else
                    {
                        peripheralData->counter--;
                    }
                    break;
            }

            if (peripheralConfig->configureTrgo)
            {
                if ((peripheralConfig->trgoSource == HW_TIM_TRGO_UPDATE) && update)
                {
                    peripheralData->triggerCount++;
                }
                else if ((peripheralConfig->trgoSource == HW_TIM_TRGO_OC_MATCH) &&
                         peripheralData->ocConfigured[0] &&
                         (peripheralData->counter == peripheralData->compare[0]))
                {
                    peripheralData->triggerCount++;
                }
                else
                {
                    // no trigger this step
                }
            }
        }
    }
}

bool HW_TIM_sim_getOutputEnabled(HW_TIM_channels_E channel)
{
    bool enabled = false;
    if ((data->initialized) &&
        (channel < HW_TIM_CHANNEL_COUNT) &&
        ((size_t)channel < data->config->numChannels))
    {
        const HW_TIM_channelConfig_S * const channelConfig = &data->config->channels[channel];
        enabled = data->peripheralData[channelConfig->peripheral].outputEnabled[channelConfig->ocUnit];
    }
    return enabled;
}

uint32_t HW_TIM_sim_getOutputLevel(HW_TIM_channels_E channel)
{
    uint32_t level = 0U;
    if ((data->initialized) &&
        (channel < HW_TIM_CHANNEL_COUNT) &&
        ((size_t)channel < data->config->numChannels))
    {
        const HW_TIM_channelConfig_S * const channelConfig = &data->config->channels[channel];
        const HW_TIM_peripheralData_S * const peripheralData =
            &data->peripheralData[channelConfig->peripheral];
        const uint8_t ocUnit = channelConfig->ocUnit;

        level = channelConfig->inactiveLevel;
        if (peripheralData->outputEnabled[ocUnit] && peripheralData->mainOutputEnabled)
        {
            level = (peripheralData->counter < peripheralData->compare[ocUnit])
                        ? HW_TIM_private_activeLevel(channelConfig->inactiveLevel)
                        : channelConfig->inactiveLevel;
        }
    }
    return level;
}

uint32_t HW_TIM_sim_getComplementaryLevel(HW_TIM_channels_E channel)
{
    uint32_t level = 0U;
    if ((data->initialized) &&
        (channel < HW_TIM_CHANNEL_COUNT) &&
        ((size_t)channel < data->config->numChannels))
    {
        const HW_TIM_channelConfig_S * const channelConfig = &data->config->channels[channel];
        const HW_TIM_peripheralData_S * const peripheralData =
            &data->peripheralData[channelConfig->peripheral];
        const uint8_t ocUnit = channelConfig->ocUnit;

        level = channelConfig->inactiveLevel;
        if (peripheralData->outputEnabled[ocUnit] && peripheralData->mainOutputEnabled)
        {
            // Antiphase to the primary output.
            level = (peripheralData->counter < peripheralData->compare[ocUnit])
                        ? channelConfig->inactiveLevel
                        : HW_TIM_private_activeLevel(channelConfig->inactiveLevel);
        }
    }
    return level;
}

uint32_t HW_TIM_sim_getDeadTime(HW_TIM_peripheral_E peripheral)
{
    uint32_t deadTime = 0U;
    if ((data->initialized) &&
        (peripheral < HW_TIM_PERIPHERAL_COUNT) &&
        ((size_t)peripheral < data->config->numPeripherals) &&
        (data->config->peripherals[peripheral].configureBreakDeadTime))
    {
        deadTime = data->config->peripherals[peripheral].deadTime;
    }
    return deadTime;
}

uint32_t HW_TIM_sim_getTriggerCount(HW_TIM_peripheral_E peripheral)
{
    uint32_t count = 0U;
    if ((data->initialized) && (peripheral < HW_TIM_PERIPHERAL_COUNT))
    {
        count = data->peripheralData[peripheral].triggerCount;
    }
    return count;
}

void HW_TIM_sim_clearTriggers(HW_TIM_peripheral_E peripheral)
{
    if ((data->initialized) && (peripheral < HW_TIM_PERIPHERAL_COUNT))
    {
        data->peripheralData[peripheral].triggerCount = 0U;
    }
}

// [impl->fw~hal_tim_007~1]
// [impl->fw~hal_tim_008~1]
void HW_TIM_sim_assertBreak(HW_TIM_peripheral_E peripheral, bool asserted)
{
    // A break assertion clears the MOE latch, as hardware does when
    // AutomaticOutput is disabled. Release does not restore it — the latch
    // stays clear until HW_TIM_setMainOutputEnabled sets it again.
    if ((data->initialized) && (peripheral < HW_TIM_PERIPHERAL_COUNT) && asserted)
    {
        data->peripheralData[peripheral].mainOutputEnabled = false;
    }
}
