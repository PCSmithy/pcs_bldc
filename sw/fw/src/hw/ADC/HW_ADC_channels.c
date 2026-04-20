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

/* Private Data Definitions */

const HW_ADC_channelConfig_S HW_ADC_channelConfig[HW_ADC_CHANNEL_COUNT] =
{
#if (BUILD_TARGET == BUILD_TARGET_STM32G4)
    [HW_ADC_CHANNEL_1] =
    {
        // ADC1: master of the ADC1+2 dual pair.
        // hadc.Init: NbrOfConversion / ScanConvMode / EOCSelection /
        // ContinuousConvMode / ExternalTrigConv are overridden by
        // HW_ADC_init based on enabled-input count + trigger/xfer modes.
        // Values left here for legibility; they're benign.
        .hadc =
        {
            .Instance                   = ADC1,
            .Init.ClockPrescaler        = ADC_CLOCK_SYNC_PCLK_DIV4,
            .Init.Resolution            = ADC_RESOLUTION_12B,
            .Init.DataAlign             = ADC_DATAALIGN_RIGHT,
            .Init.GainCompensation      = 0,
            .Init.ScanConvMode          = ADC_SCAN_DISABLE,
            .Init.EOCSelection          = ADC_EOC_SINGLE_CONV,
            .Init.LowPowerAutoWait      = DISABLE,
            .Init.ContinuousConvMode    = DISABLE,
            .Init.NbrOfConversion       = 1,
            .Init.DiscontinuousConvMode = DISABLE,
            .Init.ExternalTrigConv      = ADC_SOFTWARE_START,
            .Init.ExternalTrigConvEdge  = ADC_EXTERNALTRIGCONVEDGE_NONE,
            .Init.DMAContinuousRequests = DISABLE,
            .Init.Overrun               = ADC_OVR_DATA_PRESERVED,
            .Init.OversamplingMode      = DISABLE,
        },

        .configureMultimode = true,
        .multimode = { .Mode = ADC_MODE_INDEPENDENT, },

        .triggerMode         = HW_ADC_TRIGGER_SOFTWARE,
        .xferMode            = HW_ADC_XFER_POLLED,
        .injectedTriggerMode = HW_ADC_TRIGGER_SOFTWARE,
        .injectedXferMode    = HW_ADC_XFER_POLLED,
        .vref                = 3.3f,

        .inputs =
        {
            // ADC1_IN6 on PC0 — see HAL_ADC_MspInit GPIO setup.
            [6] =
            {
                .enabled = true,
                .sConfig =
                {
                    .Channel      = ADC_CHANNEL_6,
                    .Rank         = ADC_REGULAR_RANK_1,
                    .SamplingTime = ADC_SAMPLETIME_2CYCLES_5,
                    .SingleDiff   = ADC_SINGLE_ENDED,
                    .OffsetNumber = ADC_OFFSET_NONE,
                    .Offset       = 0,
                },
            },
            // Other inputs (IN1, IN7, IN8, IN9, IN12) have GPIO
            // configured by MspInit but are not yet enabled in the
            // regular sequence. Add slots like the IN6 block above
            // (with rank ADC_REGULAR_RANK_2, _3, ...) when needed.
        },

        .injectedInputs = { 0 },  // No injected inputs configured yet.
                                  // FOC current sensing will put phase U on
                                  // this channel's slot 0, with ADC2's slot 0
                                  // = phase V, both triggered simultaneously
                                  // by TIM1 in dual-injected mode (multimode
                                  // becomes ADC_DUALMODE_INJECSIMULT). One
                                  // injected slot per ADC; the dual-mode
                                  // pairing is what gets the "exact same
                                  // instant" sampling that FOC needs.
    },

    [HW_ADC_CHANNEL_2] =
    {
        // ADC2: slave of the ADC1+2 dual pair.
        .hadc =
        {
            .Instance                   = ADC2,
            .Init.ClockPrescaler        = ADC_CLOCK_SYNC_PCLK_DIV4,
            .Init.Resolution            = ADC_RESOLUTION_12B,
            .Init.DataAlign             = ADC_DATAALIGN_RIGHT,
            .Init.GainCompensation      = 0,
            .Init.ScanConvMode          = ADC_SCAN_DISABLE,
            .Init.EOCSelection          = ADC_EOC_SINGLE_CONV,
            .Init.LowPowerAutoWait      = DISABLE,
            .Init.ContinuousConvMode    = DISABLE,
            .Init.NbrOfConversion       = 1,
            .Init.DiscontinuousConvMode = DISABLE,
            .Init.ExternalTrigConv      = ADC_SOFTWARE_START,
            .Init.ExternalTrigConvEdge  = ADC_EXTERNALTRIGCONVEDGE_NONE,
            .Init.DMAContinuousRequests = DISABLE,
            .Init.Overrun               = ADC_OVR_DATA_PRESERVED,
            .Init.OversamplingMode      = DISABLE,
        },

        .configureMultimode = false,
        .multimode = { 0U },

        .triggerMode         = HW_ADC_TRIGGER_SOFTWARE,
        .xferMode            = HW_ADC_XFER_POLLED,
        .injectedTriggerMode = HW_ADC_TRIGGER_SOFTWARE,
        .injectedXferMode    = HW_ADC_XFER_POLLED,
        .vref                = 3.3f,

        .inputs =
        {
            // ADC2_IN11 on PC5 — see HAL_ADC_MspInit GPIO setup.
            [11] =
            {
                .enabled = true,
                .sConfig =
                {
                    .Channel      = ADC_CHANNEL_11,
                    .Rank         = ADC_REGULAR_RANK_1,
                    .SamplingTime = ADC_SAMPLETIME_2CYCLES_5,
                    .SingleDiff   = ADC_SINGLE_ENDED,
                    .OffsetNumber = ADC_OFFSET_NONE,
                    .Offset       = 0,
                },
            },
        },

        .injectedInputs = { 0 },
    },
#elif (BUILD_TARGET == BUILD_TARGET_SIM)
    [HW_ADC_CHANNEL_1] =
    {
        .channelNameStr      = "ADC1",
        .triggerMode         = HW_ADC_TRIGGER_SOFTWARE,
        .xferMode            = HW_ADC_XFER_POLLED,
        .injectedTriggerMode = HW_ADC_TRIGGER_SOFTWARE,
        .injectedXferMode    = HW_ADC_XFER_POLLED,
        .vref                = 3.3f,
        .numBits             = 12U,
        .inputs =
        {
            [6] = { .enabled = true, .inputNameStr = "ADC1_IN6 (PC0)" },
        },
        .injectedInputs = { 0 },
    },

    [HW_ADC_CHANNEL_2] =
    {
        .channelNameStr      = "ADC2",
        .triggerMode         = HW_ADC_TRIGGER_SOFTWARE,
        .xferMode            = HW_ADC_XFER_POLLED,
        .injectedTriggerMode = HW_ADC_TRIGGER_SOFTWARE,
        .injectedXferMode    = HW_ADC_XFER_POLLED,
        .vref                = 3.3f,
        .numBits             = 12U,
        .inputs =
        {
            [11] = { .enabled = true, .inputNameStr = "ADC2_IN11 (PC5)" },
        },
        .injectedInputs = { 0 },
    },
#else
#error "ERROR! HW_ADC_channelConfig not defined for build target!"
#endif
};

const HW_ADC_config_S HW_ADC_config =
{
    .channels    = HW_ADC_channelConfig,
    .numChannels = COUNTOF(HW_ADC_channelConfig),
};

/* Private Function Definitions */

/* Public Function Definitions */
