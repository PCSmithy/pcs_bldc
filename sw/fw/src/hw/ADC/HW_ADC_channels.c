
/* Includes */

#include "lib_types.h"
#include "lib_utils.h"
#include "lib_build.h"

#include "HW_ADC.h"
// HAL types in the BUILD_TARGET_STM32G4 branch (ADC_HandleTypeDef, ADC1,
// ADC_CHANNEL_6, etc.) come transitively through HW_ADC.h, which on the
// embedded target resolves to stm32g4/HW_ADC.h (which #includes
// stm32g4xx_hal.h). On the native target the elided branch is never
// parsed so no HAL include is needed.

/* Defines */

/* Typedefs */

/* Private Function Declarations */

const HW_ADC_channelConfig_S HW_ADC_channelConfig[HW_ADC_CHANNEL_COUNT] =
{
#if (BUILD_TARGET == BUILD_TARGET_STM32G4)
    [HW_ADC_CHANNEL_1] =
    {
        // TODO - update convert_cubemx_to_canonical.sh to populate this struct directly from cubemx generated output
        .hadc =
        {
            .Instance = ADC1,
            .Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4,
            .Init.Resolution = ADC_RESOLUTION_12B,
            .Init.DataAlign = ADC_DATAALIGN_RIGHT,
            .Init.GainCompensation = 0,
            .Init.ScanConvMode = ADC_SCAN_DISABLE,
            .Init.EOCSelection = ADC_EOC_SINGLE_CONV,
            .Init.LowPowerAutoWait = DISABLE,
            .Init.ContinuousConvMode = DISABLE,
            .Init.NbrOfConversion = 1,
            .Init.DiscontinuousConvMode = DISABLE,
            .Init.ExternalTrigConv = ADC_SOFTWARE_START,
            .Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE,
            .Init.DMAContinuousRequests = DISABLE,
            .Init.Overrun = ADC_OVR_DATA_PRESERVED,
            .Init.OversamplingMode = DISABLE,
        },
        .configureMultimode = true,
        .multimode =
        {
            .Mode = ADC_MODE_INDEPENDENT,
        },
        .sConfig =
        {
            .Channel = ADC_CHANNEL_6,
            .Rank = ADC_REGULAR_RANK_1,
            .SamplingTime = ADC_SAMPLETIME_2CYCLES_5,
            .SingleDiff = ADC_SINGLE_ENDED,
            .OffsetNumber = ADC_OFFSET_NONE,
            .Offset = 0,
        }
    },
    [HW_ADC_CHANNEL_2] =
    {
        // TODO - update convert_cubemx_to_canonical.sh to populate this struct directly from cubemx generated output
        .hadc =
        {
            .Instance = ADC2,
            .Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4,
            .Init.Resolution = ADC_RESOLUTION_12B,
            .Init.DataAlign = ADC_DATAALIGN_RIGHT,
            .Init.GainCompensation = 0,
            .Init.ScanConvMode = ADC_SCAN_DISABLE,
            .Init.EOCSelection = ADC_EOC_SINGLE_CONV,
            .Init.LowPowerAutoWait = DISABLE,
            .Init.ContinuousConvMode = DISABLE,
            .Init.NbrOfConversion = 1,
            .Init.DiscontinuousConvMode = DISABLE,
            .Init.ExternalTrigConv = ADC_SOFTWARE_START,
            .Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE,
            .Init.DMAContinuousRequests = DISABLE,
            .Init.Overrun = ADC_OVR_DATA_PRESERVED,
            .Init.OversamplingMode = DISABLE,
        },
        .configureMultimode = false,
        .multimode = { 0U },
        .sConfig =
        {
            .Channel = ADC_CHANNEL_11,
            .Rank = ADC_REGULAR_RANK_1,
            .SamplingTime = ADC_SAMPLETIME_2CYCLES_5,
            .SingleDiff = ADC_SINGLE_ENDED,
            .OffsetNumber = ADC_OFFSET_NONE,
            .Offset = 0,
        }
    },
#elif (BUILD_TARGET == BUILD_TARGET_SIM)
    [HW_ADC_CHANNEL_1] = { .channelNameStr = "ADC1", },
    [HW_ADC_CHANNEL_2] = { .channelNameStr = "ADC2", },
#else
#error "ERROR! HW_ADC_channelConfig not defined for build target!"
#endif
};

const HW_ADC_config_S HW_ADC_config =
{
    .channels = HW_ADC_channelConfig,
    .numChannels = COUNTOF(HW_ADC_channelConfig),
};

/* Private Data Definitions */

/* Private Function Definitions */

/* Public Function Definitions */
