/* Includes */

#include "lib_types.h"

#include "HW_TIM.h"
#include "HW_TIM_sim.h"

/* Defines */

/* Typedefs */

typedef struct
{
    uint32_t counter;
    bool     centerGoingUp;                          // direction within a center-aligned ramp
    uint32_t compare[HW_TIM_OC_UNITS_PER_CHANNEL];
    bool     outputEnabled[HW_TIM_OC_UNITS_PER_CHANNEL];
    bool     breakAsserted;
    uint32_t triggerCount;
} HW_TIM_channelData_S;

typedef struct
{
    const HW_TIM_config_S * config;
    HW_TIM_channelData_S channelData[HW_TIM_CHANNEL_COUNT];
    bool initialized;
} HW_TIM_data_S;

/* Private Data Definitions */

static HW_TIM_data_S HW_TIM_data;
static HW_TIM_data_S * const data = &HW_TIM_data;

/* Private Function Declarations */

static uint32_t HW_TIM_private_maxForWidth(uint32_t widthBits);
static bool HW_TIM_private_validateChannel(const HW_TIM_channelConfig_S * const channelConfig);
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

static bool HW_TIM_private_validateChannel(const HW_TIM_channelConfig_S * const channelConfig)
{
    bool valid = ((channelConfig->countDir == HW_TIM_COUNT_UP)   ||
                  (channelConfig->countDir == HW_TIM_COUNT_DOWN) ||
                  (channelConfig->countDir == HW_TIM_COUNT_CENTER));

    if (valid && (channelConfig->period > HW_TIM_private_maxForWidth(channelConfig->counterWidthBits)))
    {
        valid = false;
    }

    for (uint8_t unit = 0U; (unit < HW_TIM_OC_UNITS_PER_CHANNEL) && valid; unit++)
    {
        const HW_TIM_ocConfig_S * const ocConfig = &channelConfig->outputCompare[unit];
        if (ocConfig->enabled && (ocConfig->compare > channelConfig->period))
        {
            valid = false;
        }
    }

    if (valid && channelConfig->configureBreakDeadTime && (channelConfig->deadTime > 0xFFU))
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
        (config->channels != NULL) &&
        (config->numChannels <= HW_TIM_CHANNEL_COUNT))
    {
        bool valid = true;
        for (size_t ch = 0U; (ch < config->numChannels) && valid; ch++)
        {
            valid = HW_TIM_private_validateChannel(&config->channels[ch]);
        }

        if (valid)
        {
            data->config = config;
            for (size_t ch = 0U; ch < config->numChannels; ch++)
            {
                const HW_TIM_channelConfig_S * const channelConfig = &config->channels[ch];
                HW_TIM_channelData_S * const channelData = &data->channelData[ch];

                *channelData = (HW_TIM_channelData_S){ 0 };
                channelData->centerGoingUp = true;
                channelData->counter =
                    (channelConfig->countDir == HW_TIM_COUNT_DOWN) ? channelConfig->period : 0U;
                for (uint8_t unit = 0U; unit < HW_TIM_OC_UNITS_PER_CHANNEL; unit++)
                {
                    channelData->compare[unit] = channelConfig->outputCompare[unit].compare;
                }
            }
            data->initialized = true;
            ret = true;
        }
    }
    return ret;
}

// [impl->fw~hal_tim_003~1]
bool HW_TIM_getCounter(HW_TIM_channels_E channel, uint32_t * const out)
{
    bool ret = false;
    if ((out != NULL) &&
        (data->initialized) &&
        (channel < HW_TIM_CHANNEL_COUNT) &&
        (channel < data->config->numChannels))
    {
        *out = data->channelData[channel].counter;
        ret = true;
    }
    return ret;
}

// [impl->fw~hal_tim_004~1]
bool HW_TIM_setCompare(HW_TIM_channels_E channel, uint8_t ocUnit, uint32_t counts)
{
    bool ret = false;
    if ((data->initialized) &&
        (channel < HW_TIM_CHANNEL_COUNT) &&
        (channel < data->config->numChannels) &&
        (ocUnit < HW_TIM_OC_UNITS_PER_CHANNEL))
    {
        const HW_TIM_channelConfig_S * const channelConfig = &data->config->channels[channel];
        if (channelConfig->outputCompare[ocUnit].enabled && (counts <= channelConfig->period))
        {
            data->channelData[channel].compare[ocUnit] = counts;
            ret = true;
        }
    }
    return ret;
}

// [impl->fw~hal_tim_004~1]
bool HW_TIM_getCompare(HW_TIM_channels_E channel, uint8_t ocUnit, uint32_t * const out)
{
    bool ret = false;
    if ((out != NULL) &&
        (data->initialized) &&
        (channel < HW_TIM_CHANNEL_COUNT) &&
        (channel < data->config->numChannels) &&
        (ocUnit < HW_TIM_OC_UNITS_PER_CHANNEL) &&
        (data->config->channels[channel].outputCompare[ocUnit].enabled))
    {
        *out = data->channelData[channel].compare[ocUnit];
        ret = true;
    }
    return ret;
}

// [impl->fw~hal_tim_004~1]
bool HW_TIM_setOutputEnabled(HW_TIM_channels_E channel, uint8_t ocUnit, bool enabled)
{
    bool ret = false;
    if ((data->initialized) &&
        (channel < HW_TIM_CHANNEL_COUNT) &&
        (channel < data->config->numChannels) &&
        (ocUnit < HW_TIM_OC_UNITS_PER_CHANNEL) &&
        (data->config->channels[channel].outputCompare[ocUnit].enabled))
    {
        data->channelData[channel].outputEnabled[ocUnit] = enabled;
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
void HW_TIM_sim_advance(HW_TIM_channels_E channel, uint32_t ticks)
{
    if ((data->initialized) &&
        (channel < HW_TIM_CHANNEL_COUNT) &&
        (channel < data->config->numChannels))
    {
        const HW_TIM_channelConfig_S * const channelConfig = &data->config->channels[channel];
        HW_TIM_channelData_S * const channelData = &data->channelData[channel];
        const uint32_t period = channelConfig->period;

        for (uint32_t step = 0U; step < ticks; step++)
        {
            bool update = false;

            switch (channelConfig->countDir)
            {
                case HW_TIM_COUNT_UP:
                    if (channelData->counter >= period)
                    {
                        channelData->counter = 0U;
                        update = true;
                    }
                    else
                    {
                        channelData->counter++;
                    }
                    break;

                case HW_TIM_COUNT_DOWN:
                    if (channelData->counter == 0U)
                    {
                        channelData->counter = period;
                        update = true;
                    }
                    else
                    {
                        channelData->counter--;
                    }
                    break;

                case HW_TIM_COUNT_CENTER:
                default:
                    if (channelData->centerGoingUp)
                    {
                        channelData->counter++;
                        if (channelData->counter >= period)
                        {
                            channelData->counter = period;
                            channelData->centerGoingUp = false;
                        }
                    }
                    else if (channelData->counter == 0U)
                    {
                        channelData->centerGoingUp = true;
                        update = true;
                    }
                    else
                    {
                        channelData->counter--;
                    }
                    break;
            }

            if (channelConfig->configureTrgo)
            {
                if ((channelConfig->trgoSource == HW_TIM_TRGO_UPDATE) && update)
                {
                    channelData->triggerCount++;
                }
                else if ((channelConfig->trgoSource == HW_TIM_TRGO_OC_MATCH) &&
                         channelConfig->outputCompare[0].enabled &&
                         (channelData->counter == channelData->compare[0]))
                {
                    channelData->triggerCount++;
                }
                else
                {
                    // no trigger this step
                }
            }
        }
    }
}

bool HW_TIM_sim_getOutputEnabled(HW_TIM_channels_E channel, uint8_t ocUnit)
{
    bool enabled = false;
    if ((data->initialized) &&
        (channel < HW_TIM_CHANNEL_COUNT) &&
        (ocUnit < HW_TIM_OC_UNITS_PER_CHANNEL))
    {
        enabled = data->channelData[channel].outputEnabled[ocUnit];
    }
    return enabled;
}

uint32_t HW_TIM_sim_getOutputLevel(HW_TIM_channels_E channel, uint8_t ocUnit)
{
    uint32_t level = 0U;
    if ((data->initialized) &&
        (channel < HW_TIM_CHANNEL_COUNT) &&
        (channel < data->config->numChannels) &&
        (ocUnit < HW_TIM_OC_UNITS_PER_CHANNEL))
    {
        const HW_TIM_ocConfig_S * const ocConfig =
            &data->config->channels[channel].outputCompare[ocUnit];
        const HW_TIM_channelData_S * const channelData = &data->channelData[channel];

        level = ocConfig->inactiveLevel;
        if (channelData->outputEnabled[ocUnit] && !channelData->breakAsserted)
        {
            level = (channelData->counter < channelData->compare[ocUnit])
                        ? HW_TIM_private_activeLevel(ocConfig->inactiveLevel)
                        : ocConfig->inactiveLevel;
        }
    }
    return level;
}

uint32_t HW_TIM_sim_getComplementaryLevel(HW_TIM_channels_E channel, uint8_t ocUnit)
{
    uint32_t level = 0U;
    if ((data->initialized) &&
        (channel < HW_TIM_CHANNEL_COUNT) &&
        (channel < data->config->numChannels) &&
        (ocUnit < HW_TIM_OC_UNITS_PER_CHANNEL))
    {
        const HW_TIM_ocConfig_S * const ocConfig =
            &data->config->channels[channel].outputCompare[ocUnit];
        const HW_TIM_channelData_S * const channelData = &data->channelData[channel];

        level = ocConfig->inactiveLevel;
        if (channelData->outputEnabled[ocUnit] && !channelData->breakAsserted)
        {
            // Antiphase to the primary output.
            level = (channelData->counter < channelData->compare[ocUnit])
                        ? ocConfig->inactiveLevel
                        : HW_TIM_private_activeLevel(ocConfig->inactiveLevel);
        }
    }
    return level;
}

uint32_t HW_TIM_sim_getDeadTime(HW_TIM_channels_E channel)
{
    uint32_t deadTime = 0U;
    if ((data->initialized) &&
        (channel < HW_TIM_CHANNEL_COUNT) &&
        (channel < data->config->numChannels) &&
        (data->config->channels[channel].configureBreakDeadTime))
    {
        deadTime = data->config->channels[channel].deadTime;
    }
    return deadTime;
}

uint32_t HW_TIM_sim_getTriggerCount(HW_TIM_channels_E channel)
{
    uint32_t count = 0U;
    if ((data->initialized) && (channel < HW_TIM_CHANNEL_COUNT))
    {
        count = data->channelData[channel].triggerCount;
    }
    return count;
}

void HW_TIM_sim_clearTriggers(HW_TIM_channels_E channel)
{
    if ((data->initialized) && (channel < HW_TIM_CHANNEL_COUNT))
    {
        data->channelData[channel].triggerCount = 0U;
    }
}

// [impl->fw~hal_tim_007~1]
void HW_TIM_sim_assertBreak(HW_TIM_channels_E channel, bool asserted)
{
    if ((data->initialized) && (channel < HW_TIM_CHANNEL_COUNT))
    {
        data->channelData[channel].breakAsserted = asserted;
    }
}
