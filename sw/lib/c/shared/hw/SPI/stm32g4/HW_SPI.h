#ifndef HW_SPI_H
#define HW_SPI_H

/* Includes */
#include "lib_types.h"
#include "stm32g4xx_hal.h"

#include "HW_SPI_channels.h"

/* Defines */

/* Typedefs */

typedef enum
{
    HW_SPI_TRANSFERMODE_SW,
    HW_SPI_TRANSFERMODE_INTERRUPT,
    HW_SPI_TRANSFERMODE_DMA,
} HW_SPI_transferMode_E;

typedef struct
{
    bool enabled;
    SPI_HandleTypeDef hspi;

    HW_SPI_transferMode_E transferMode;
} HW_SPI_channelConfig_S;

typedef struct
{
    const HW_SPI_channelConfig_S * channels;
    size_t numChannels;
} HW_SPI_config_S;

/* Static Inline Functions */

/* Public Function Declarations */

bool HW_SPI_init(const HW_SPI_config_S * const config);

bool HW_SPI_transmit(HW_SPI_channels_E channel, uint8_t * txData, size_t length);
bool HW_SPI_receive(HW_SPI_channels_E channel, uint8_t * rxData, size_t length);
bool HW_SPI_transmitReceive(HW_SPI_channels_E channel, uint8_t * txData, uint8_t * rxData, size_t length);

#endif // HW_SPI_H
