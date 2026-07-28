/* Includes */

#include "lib_types.h"
#include "lib_utils.h"

#include "HW_TIM.h"

#include "HW_TIM_channels.cubemx.h"

/* Private Data Definitions */

const HW_TIM_peripheralConfig_S HW_TIM_peripheralConfig[HW_TIM_PERIPHERAL_COUNT] =
{
    [HW_TIM_PERIPHERAL_1] = { HW_TIM_CUBEMX_PERIPH_TIM1 },   // 3-phase motor PWM
    [HW_TIM_PERIPHERAL_2] = { HW_TIM_CUBEMX_PERIPH_TIM2 },   // 1 us time base
};

const HW_TIM_channelConfig_S HW_TIM_channelConfig[HW_TIM_CHANNEL_COUNT] =
{
    [HW_TIM_CHANNEL_PWM_U] =
    {
        .peripheral = HW_TIM_PERIPHERAL_1,
        .role       = HW_TIM_ROLE_OUTPUT_COMPARE,
        .ocUnit     = 0U,
        HW_TIM_CUBEMX_OC_TIM1_CH1
#if (BUILD_TARGET == BUILD_TARGET_SIM)
        .channelNameStr = "PWM_U",
#endif
    },
    [HW_TIM_CHANNEL_PWM_V] =
    {
        .peripheral = HW_TIM_PERIPHERAL_1,
        .role       = HW_TIM_ROLE_OUTPUT_COMPARE,
        .ocUnit     = 1U,
        HW_TIM_CUBEMX_OC_TIM1_CH2
#if (BUILD_TARGET == BUILD_TARGET_SIM)
        .channelNameStr = "PWM_V",
#endif
    },
    [HW_TIM_CHANNEL_PWM_W] =
    {
        .peripheral = HW_TIM_PERIPHERAL_1,
        .role       = HW_TIM_ROLE_OUTPUT_COMPARE,
        .ocUnit     = 2U,
        HW_TIM_CUBEMX_OC_TIM1_CH3
#if (BUILD_TARGET == BUILD_TARGET_SIM)
        .channelNameStr = "PWM_W",
#endif
    },
};

const HW_TIM_config_S HW_TIM_config =
{
    .peripherals    = HW_TIM_peripheralConfig,
    .numPeripherals = COUNTOF(HW_TIM_peripheralConfig),
    .channels       = HW_TIM_channelConfig,
    .numChannels    = COUNTOF(HW_TIM_channelConfig),
};
