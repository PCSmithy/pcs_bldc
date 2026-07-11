/* Includes */

#include "lib_types.h"

#include "HW_TIM.h"

/* Defines */

/* Typedefs */

typedef struct
{
    // Mutable HAL handles, seeded from the const config at init. HAL
    // mutates these (state, lock), so they cannot live in the const config.
    TIM_HandleTypeDef htim[HW_TIM_PERIPHERAL_COUNT];

    // Last-commanded MOE per peripheral. HAL_TIM_PWM_Start force-sets the BDTR
    // MOE bit, so setOutputEnabled re-applies this to keep enabling a CCx unit
    // from energizing outputs while the bridge is commanded off. Distinct from
    // the hardware MOE bit, which a break event can clear behind our back.
    bool moeCommanded[HW_TIM_PERIPHERAL_COUNT];

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
static bool HW_TIM_private_validatePeripheral(const HW_TIM_peripheralConfig_S * const peripheralConfig);
static bool HW_TIM_private_validateChannel(const HW_TIM_config_S * const config,
                                           const HW_TIM_channelConfig_S * const channelConfig);
static bool HW_TIM_private_initPeripheral(const HW_TIM_config_S * const config,
                                          HW_TIM_peripheral_E peripheral,
                                          TIM_HandleTypeDef * const htim);

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

static bool HW_TIM_private_validatePeripheral(const HW_TIM_peripheralConfig_S * const peripheralConfig)
{
    bool valid = HW_TIM_private_isCountModeSupported(peripheralConfig->htim.Init.CounterMode);

    // The dead-time generator field (DTG) is 8 bits wide.
    if (valid && peripheralConfig->configureBreakDeadTime &&
        (peripheralConfig->breakDeadTime.DeadTime > 0xFFU))
    {
        valid = false;
    }

    return valid;
}

static bool HW_TIM_private_validateChannel(const HW_TIM_config_S * const config,
                                           const HW_TIM_channelConfig_S * const channelConfig)
{
    bool valid = ((channelConfig->role == HW_TIM_ROLE_OUTPUT_COMPARE)      &&
                  ((size_t)channelConfig->peripheral < config->numPeripherals) &&
                  (channelConfig->ocUnit < HW_TIM_OC_UNITS_PER_PERIPHERAL));

    if (valid)
    {
        const uint32_t period = config->peripherals[channelConfig->peripheral].htim.Init.Period;
        if (channelConfig->oc.Pulse > period)
        {
            valid = false;
        }
    }

    return valid;
}

static bool HW_TIM_private_initPeripheral(const HW_TIM_config_S * const config,
                                          HW_TIM_peripheral_E peripheral,
                                          TIM_HandleTypeDef * const htim)
{
    const HW_TIM_peripheralConfig_S * const peripheralConfig = &config->peripherals[peripheral];

    // A peripheral may carry both PWM and plain output-compare channels; scan
    // its logical channels to learn which init modes are present.
    bool hasPwm = false;
    bool hasOc  = false;
    for (size_t ch = 0U; ch < config->numChannels; ch++)
    {
        const HW_TIM_channelConfig_S * const channelConfig = &config->channels[ch];
        if (channelConfig->peripheral == peripheral)
        {
            if (HW_TIM_private_isPwmMode(channelConfig->oc.OCMode))
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

    if (ret && hasPwm)
    {
        ret = (HAL_TIM_PWM_Init(htim) == HAL_OK);
    }
    if (ret && hasOc)
    {
        ret = (HAL_TIM_OC_Init(htim) == HAL_OK);
    }

    if (ret && peripheralConfig->configureTrgo)
    {
        TIM_MasterConfigTypeDef master = peripheralConfig->master;
        ret = (HAL_TIMEx_MasterConfigSynchronization(htim, &master) == HAL_OK);
    }

    for (size_t ch = 0U; (ch < config->numChannels) && ret; ch++)
    {
        const HW_TIM_channelConfig_S * const channelConfig = &config->channels[ch];
        if (channelConfig->peripheral == peripheral)
        {
            TIM_OC_InitTypeDef oc = channelConfig->oc;
            if (HW_TIM_private_isPwmMode(oc.OCMode))
            {
                ret = (HAL_TIM_PWM_ConfigChannel(htim, &oc, channelConfig->channel) == HAL_OK);
            }
            else
            {
                ret = (HAL_TIM_OC_ConfigChannel(htim, &oc, channelConfig->channel) == HAL_OK);
            }
        }
    }

    if (ret && peripheralConfig->configureBreakDeadTime)
    {
        TIM_BreakDeadTimeConfigTypeDef breakDeadTime = peripheralConfig->breakDeadTime;
        ret = (HAL_TIMEx_ConfigBreakDeadTime(htim, &breakDeadTime) == HAL_OK);
    }

    for (uint8_t i = 0U; (i < HW_TIM_BREAK_INPUTS_PER_PERIPHERAL) && ret; i++)
    {
        const HW_TIM_breakInputConfig_S * const breakInput = &peripheralConfig->breakInputs[i];
        if (breakInput->enabled)
        {
            TIMEx_BreakInputConfigTypeDef bi = breakInput->config;
            ret = (HAL_TIMEx_ConfigBreakInput(htim, breakInput->breakInput, &bi) == HAL_OK);
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
            bool initOk = true;
            for (size_t p = 0U; (p < config->numPeripherals) && initOk; p++)
            {
                data->htim[p] = config->peripherals[p].htim;
                // MOE commanded OFF at init: outputs stay dead (matching
                // fw~hal_tim_001's inactive-at-start contract) until a consumer
                // calls HW_TIM_setMainOutputEnabled.
                data->moeCommanded[p] = false;
                initOk = HW_TIM_private_initPeripheral(config, (HW_TIM_peripheral_E)p, &data->htim[p]);
            }
            data->initialized = initOk;
            ret = initOk;
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
        *out = __HAL_TIM_GET_COUNTER(&data->htim[peripheral]);
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
        *out = data->htim[peripheral].Init.Period;
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
        TIM_HandleTypeDef * const htim = &data->htim[channelConfig->peripheral];
        if (counts <= htim->Init.Period)
        {
            __HAL_TIM_SET_COMPARE(htim, channelConfig->channel, counts);
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
        *out = __HAL_TIM_GET_COMPARE(&data->htim[channelConfig->peripheral], channelConfig->channel);
        ret = true;
    }
    return ret;
}

// [impl->fw~hal_tim_004~1]
// [impl->fw~hal_tim_008~1]
bool HW_TIM_setOutputEnabled(HW_TIM_channels_E channel, bool enabled)
{
    bool ret = false;
    if ((data->initialized) &&
        (channel < HW_TIM_CHANNEL_COUNT) &&
        ((size_t)channel < data->config->numChannels))
    {
        const HW_TIM_channelConfig_S * const channelConfig = &data->config->channels[channel];
        TIM_HandleTypeDef * const htim = &data->htim[channelConfig->peripheral];
        bool ok = true;
        if (enabled)
        {
            ok = (HAL_TIM_PWM_Start(htim, channelConfig->channel) == HAL_OK);
            if (ok && channelConfig->complementary)
            {
                ok = (HAL_TIMEx_PWMN_Start(htim, channelConfig->channel) == HAL_OK);
            }
            // HAL_TIM_PWM_Start / _PWMN_Start unconditionally set MOE on
            // advanced timers. MOE is owned by setMainOutputEnabled, so
            // clear it back when the bridge is commanded off — otherwise
            // enabling a CCx unit would energize outputs. The unconditional
            // clear is required because the CCx channel is now enabled, so
            // the guarded __HAL_TIM_MOE_DISABLE would decline to act.
            if (ok && !data->moeCommanded[channelConfig->peripheral])
            {
                __HAL_TIM_MOE_DISABLE_UNCONDITIONALLY(htim);
            }
        }
        else
        {
            ok = (HAL_TIM_PWM_Stop(htim, channelConfig->channel) == HAL_OK);
            if (ok && channelConfig->complementary)
            {
                ok = (HAL_TIMEx_PWMN_Stop(htim, channelConfig->channel) == HAL_OK);
            }
        }
        ret = ok;
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
        TIM_HandleTypeDef * const htim = &data->htim[peripheral];
        if (enabled)
        {
            __HAL_TIM_MOE_ENABLE(htim);
        }
        else
        {
            // Unconditional: the gate must drop outputs even while the CCx
            // units stay enabled (the guarded disable would decline).
            __HAL_TIM_MOE_DISABLE_UNCONDITIONALLY(htim);
        }
        data->moeCommanded[peripheral] = enabled;
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
        // Read the live BDTR bit, not moeCommanded: a break event clears MOE in
        // hardware (AutomaticOutput is disabled, so it stays clear after the
        // break releases) and that must be visible to the caller.
        *enabled = ((data->htim[peripheral].Instance->BDTR & TIM_BDTR_MOE) != 0U);
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
        // SR flags are rc_w0: writing 0 clears a flag, writing 1 leaves it
        // unchanged, so this cannot lose a concurrently-set non-break flag.
        data->htim[peripheral].Instance->SR = ~(TIM_SR_BIF | TIM_SR_B2IF | TIM_SR_SBIF);
        ret = true;
    }
    return ret;
}
