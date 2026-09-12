/* Includes */

#include "lib_types.h"
#include "lib_utils.h"
#include "lib_build.h"

#include "HW_DMA.h"

/* Private Data Definitions */

static const HW_DMA_channelConfig_S HW_DMA_channelConfig[] =
{
    [HW_DMA_CHANNEL_SK6805_TX] =
    {
#if (BUILD_TARGET == BUILD_TARGET_STM32G4)
        // SK6805 LED frame: memory -> SPI3 TX data register, 8-bit, DMAMUX
        // request SPI3_TX. Frees the ~1.25 ms blocking LED transmit off the CPU.
        .hdma =
        {
            .Instance = DMA1_Channel1,
            .Init =
            {
                .Request             = DMA_REQUEST_SPI3_TX,
                .Direction           = DMA_MEMORY_TO_PERIPH,
                .PeriphInc           = DMA_PINC_DISABLE,
                .MemInc              = DMA_MINC_ENABLE,
                .PeriphDataAlignment = DMA_PDATAALIGN_BYTE,
                .MemDataAlignment    = DMA_MDATAALIGN_BYTE,
                .Mode                = DMA_NORMAL,
                .Priority            = DMA_PRIORITY_HIGH,
            },
        },
        .periphAddress = (uint32_t)&(SPI3->DR),
        .irqn          = DMA1_Channel1_IRQn,
#elif (BUILD_TARGET == BUILD_TARGET_SIM)
        .direction      = HW_DMA_DIRECTION_MEM_TO_PERIPH,
        .width          = HW_DMA_WIDTH_8BIT,
        .channelNameStr = "SK6805_TX",
#else
# error "ERROR! HW_DMA_config not defined for build target!"
#endif
    },
    [HW_DMA_CHANNEL_AS5048_RX] =
    {
#if (BUILD_TARGET == BUILD_TARGET_STM32G4)
        // AS5048 encoder read: SPI1 RX data register -> memory, 8-bit, DMAMUX
        // request SPI1_RX. Frees the blocking encoder read off the 1 ms task.
        .hdma =
        {
            .Instance = DMA1_Channel2,
            .Init =
            {
                .Request             = DMA_REQUEST_SPI1_RX,
                .Direction           = DMA_PERIPH_TO_MEMORY,
                .PeriphInc           = DMA_PINC_DISABLE,
                .MemInc              = DMA_MINC_ENABLE,
                .PeriphDataAlignment = DMA_PDATAALIGN_BYTE,
                .MemDataAlignment    = DMA_MDATAALIGN_BYTE,
                .Mode                = DMA_NORMAL,
                .Priority            = DMA_PRIORITY_HIGH,
            },
        },
        .periphAddress = (uint32_t)&(SPI1->DR),
        .irqn          = DMA1_Channel2_IRQn,
#elif (BUILD_TARGET == BUILD_TARGET_SIM)
        .direction      = HW_DMA_DIRECTION_PERIPH_TO_MEM,
        .width          = HW_DMA_WIDTH_8BIT,
        .channelNameStr = "AS5048_RX",
#endif
    },
    [HW_DMA_CHANNEL_ADC1_REG_CONVERSIONS] =
    {
#if (BUILD_TARGET == BUILD_TARGET_STM32G4)
        // ADC1 data register -> memory, 16-bit, DMAMUX request ADC1.
        .hdma =
        {
            .Instance = DMA1_Channel3,
            .Init =
            {
                .Request             = DMA_REQUEST_ADC1,
                .Direction           = DMA_PERIPH_TO_MEMORY,
                .PeriphInc           = DMA_PINC_DISABLE,
                .MemInc              = DMA_MINC_ENABLE,
                .PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD,
                .MemDataAlignment    = DMA_MDATAALIGN_HALFWORD,
                .Mode                = DMA_NORMAL,
                .Priority            = DMA_PRIORITY_HIGH,
            },
        },
        .periphAddress = (uint32_t)&(ADC1->DR),
        .irqn          = DMA1_Channel3_IRQn,
#elif (BUILD_TARGET == BUILD_TARGET_SIM)
        .direction      = HW_DMA_DIRECTION_PERIPH_TO_MEM,
        .width          = HW_DMA_WIDTH_16BIT,
        .channelNameStr = "ADC1_DMA",
#endif
    },
    [HW_DMA_CHANNEL_ADC2_REG_CONVERSIONS] =
    {
#if (BUILD_TARGET == BUILD_TARGET_STM32G4)
        // ADC2 data register -> memory, 16-bit, DMAMUX request ADC2.
        .hdma =
        {
            .Instance = DMA1_Channel4,
            .Init =
            {
                .Request             = DMA_REQUEST_ADC2,
                .Direction           = DMA_PERIPH_TO_MEMORY,
                .PeriphInc           = DMA_PINC_DISABLE,
                .MemInc              = DMA_MINC_ENABLE,
                .PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD,
                .MemDataAlignment    = DMA_MDATAALIGN_HALFWORD,
                .Mode                = DMA_NORMAL,
                .Priority            = DMA_PRIORITY_HIGH,
            },
        },
        .periphAddress = (uint32_t)&(ADC2->DR),
        .irqn          = DMA1_Channel4_IRQn,
#elif (BUILD_TARGET == BUILD_TARGET_SIM)
        .direction      = HW_DMA_DIRECTION_PERIPH_TO_MEM,
        .width          = HW_DMA_WIDTH_16BIT,
        .channelNameStr = "ADC2_DMA",
#endif
    },
};

const HW_DMA_config_S HW_DMA_config =
{
    .channels = HW_DMA_channelConfig,
    .numChannels = COUNTOF(HW_DMA_channelConfig),
};
