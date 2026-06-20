/* Includes */

#include "lib_types.h"

#include "HW_TIM.h"

/* Defines */

/* Typedefs */

typedef struct
{
    // Mutable HAL handles, seeded from the const config at init. HAL
    // mutates these (state, lock), so they cannot live in the const config.
    TIM_HandleTypeDef htim[HW_TIM_CHANNEL_COUNT];

    const HW_TIM_config_S * config;
    bool initialized;
} HW_TIM_data_S;

/* Private Data Definitions */

static HW_TIM_data_S HW_TIM_data;
static HW_TIM_data_S * const data = &HW_TIM_data;

// Board-provided output-pin alternate-function setup (a CubeMX convention
// invoked at the end of MX_TIMx_Init). Defined weak so a project that
// drives no timer output pins links without providing one; the board's
// stm32g4xx_hal_msp.c supplies the strong definition.
__attribute__((weak)) void HAL_TIM_MspPostInit(TIM_HandleTypeDef * htim)
{
    (void)htim;
}

/* Private Function Declarations */

static bool HW_TIM_private_isCountModeSupported(uint32_t countMode);
static bool HW_TIM_private_isPwmMode(uint32_t ocMode);
static bool HW_TIM_private_validateChannel(const HW_TIM_channelConfig_S * const channelConfig);
static bool HW_TIM_private_initChannel(TIM_HandleTypeDef * const htim,
                                       const HW_TIM_channelConfig_S * const channelConfig);

/* Private Function Definitions */

static bool HW_TIM_private_isCountModeSupported(uint32_t countMode)
{
    return ((countMode == TIM_COUNTERMODE_UP)             ||
            (countMode == TIM_COUNTERMODE_DOWN)           ||
            (countMode == TIM_COUNTERMODE_CENTERALIGNED1) ||
            (countMode == TIM_COUNTERMODE_CENTERALIGNED2) ||
            (countMode == TIM_COUNTERMODE_CENTERALIGNED3));
}

static bool HW_TIM_private_isPwmMode(uint32_t ocMode)
{
    return ((ocMode == TIM_OCMODE_PWM1) || (ocMode == TIM_OCMODE_PWM2));
}

static bool HW_TIM_private_validateChannel(const HW_TIM_channelConfig_S * const channelConfig)
{
    bool valid = HW_TIM_private_isCountModeSupported(channelConfig->htim.Init.CounterMode);

    for (uint8_t unit = 0U; (unit < HW_TIM_OC_UNITS_PER_CHANNEL) && valid; unit++)
    {
        const HW_TIM_ocConfig_S * const ocConfig = &channelConfig->outputCompare[unit];
        if (ocConfig->enabled && (ocConfig->oc.Pulse > channelConfig->htim.Init.Period))
        {
            valid = false;
        }
    }

    // The dead-time generator field (DTG) is 8 bits wide.
    if (valid && channelConfig->configureBreakDeadTime &&
        (channelConfig->breakDeadTime.DeadTime > 0xFFU))
    {
        valid = false;
    }

    return valid;
}

static bool HW_TIM_private_initChannel(TIM_HandleTypeDef * const htim,
                                       const HW_TIM_channelConfig_S * const channelConfig)
{
    bool hasPwm = false;
    bool hasOc  = false;
    for (uint8_t unit = 0U; unit < HW_TIM_OC_UNITS_PER_CHANNEL; unit++)
    {
        const HW_TIM_ocConfig_S * const ocConfig = &channelConfig->outputCompare[unit];
        if (ocConfig->enabled)
        {
            if (HW_TIM_private_isPwmMode(ocConfig->oc.OCMode))
            {
                hasPwm = true;
            }
            else
            {
                hasOc = true;
            }
        }
    }

    bool ret = (HAL_TIM_Base_Init(htim) == HAL_OK);

    if (ret)
    {
        TIM_ClockConfigTypeDef clockSource = { 0 };
        clockSource.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
        ret = (HAL_TIM_ConfigClockSource(htim, &clockSource) == HAL_OK);
    }

    // A timer may carry both PWM and plain output-compare channels; init
    // each mode that is present.
    if (ret && hasPwm)
    {
        ret = (HAL_TIM_PWM_Init(htim) == HAL_OK);
    }
    if (ret && hasOc)
    {
        ret = (HAL_TIM_OC_Init(htim) == HAL_OK);
    }

    if (ret && channelConfig->configureTrgo)
    {
        TIM_MasterConfigTypeDef master = channelConfig->master;
        ret = (HAL_TIMEx_MasterConfigSynchronization(htim, &master) == HAL_OK);
    }

    for (uint8_t unit = 0U; (unit < HW_TIM_OC_UNITS_PER_CHANNEL) && ret; unit++)
    {
        const HW_TIM_ocConfig_S * const ocConfig = &channelConfig->outputCompare[unit];
        if (ocConfig->enabled)
        {
            TIM_OC_InitTypeDef oc = ocConfig->oc;
            if (HW_TIM_private_isPwmMode(oc.OCMode))
            {
                ret = (HAL_TIM_PWM_ConfigChannel(htim, &oc, ocConfig->channel) == HAL_OK);
            }
            else
            {
                ret = (HAL_TIM_OC_ConfigChannel(htim, &oc, ocConfig->channel) == HAL_OK);
            }
        }
    }

    if (ret && channelConfig->configureBreakDeadTime)
    {
        TIM_BreakDeadTimeConfigTypeDef breakDeadTime = channelConfig->breakDeadTime;
        ret = (HAL_TIMEx_ConfigBreakDeadTime(htim, &breakDeadTime) == HAL_OK);
    }

    for (uint8_t i = 0U; (i < HW_TIM_BREAK_INPUTS_PER_CHANNEL) && ret; i++)
    {
        const HW_TIM_breakInputConfig_S * const breakInput = &channelConfig->breakInputs[i];
        if (breakInput->enabled)
        {
            TIMEx_BreakInputConfigTypeDef config = breakInput->config;
            ret = (HAL_TIMEx_ConfigBreakInput(htim, breakInput->breakInput, &config) == HAL_OK);
        }
    }

    // Wire the output pins' alternate functions (board MSP), then start the
    // counter. Outputs stay disabled until a consumer enables them via
    // HW_TIM_setOutputEnabled.
    if (ret)
    {
        HAL_TIM_MspPostInit(htim);
        ret = (HAL_TIM_Base_Start(htim) == HAL_OK);
    }

    return ret;
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
            bool initOk = true;
            for (size_t ch = 0U; (ch < config->numChannels) && initOk; ch++)
            {
                data->htim[ch] = config->channels[ch].htim;
                initOk = HW_TIM_private_initChannel(&data->htim[ch], &config->channels[ch]);
            }
            data->initialized = initOk;
            ret = initOk;
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
        *out = __HAL_TIM_GET_COUNTER(&data->htim[channel]);
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
        const HW_TIM_ocConfig_S * const ocConfig = &channelConfig->outputCompare[ocUnit];
        if (ocConfig->enabled && (counts <= channelConfig->htim.Init.Period))
        {
            __HAL_TIM_SET_COMPARE(&data->htim[channel], ocConfig->channel, counts);
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
        const uint32_t halChannel = data->config->channels[channel].outputCompare[ocUnit].channel;
        *out = __HAL_TIM_GET_COMPARE(&data->htim[channel], halChannel);
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
        (ocUnit < HW_TIM_OC_UNITS_PER_CHANNEL))
    {
        const HW_TIM_ocConfig_S * const ocConfig =
            &data->config->channels[channel].outputCompare[ocUnit];
        if (ocConfig->enabled)
        {
            TIM_HandleTypeDef * const htim = &data->htim[channel];
            bool ok = true;
            if (enabled)
            {
                ok = (HAL_TIM_PWM_Start(htim, ocConfig->channel) == HAL_OK);
                if (ok && ocConfig->complementary)
                {
                    ok = (HAL_TIMEx_PWMN_Start(htim, ocConfig->channel) == HAL_OK);
                }
            }
            else
            {
                ok = (HAL_TIM_PWM_Stop(htim, ocConfig->channel) == HAL_OK);
                if (ok && ocConfig->complementary)
                {
                    ok = (HAL_TIMEx_PWMN_Stop(htim, ocConfig->channel) == HAL_OK);
                }
            }
            ret = ok;
        }
    }
    return ret;
}
