#ifndef HW_SPI_H
#define HW_SPI_H

/* Includes */
#include "lib_types.h"

#include "HW_SPI_channels.h"

/* Defines */

/* Typedefs */

typedef struct
{
    bool enabled;
    char * busNameStr;
} HW_SPI_busConfig_S;

typedef struct
{
    HW_SPI_bus_E bus;
    char * channelNameStr;
} HW_SPI_channelConfig_S;

typedef struct
{
    const HW_SPI_busConfig_S * buses;
    size_t numBuses;

    const HW_SPI_channelConfig_S * channels;
    size_t numChannels;
} HW_SPI_config_S;

/* Static Inline Functions */

/* Public Function Declarations */

bool HW_SPI_init(const HW_SPI_config_S * const config);

bool HW_SPI_transmit(HW_SPI_channel_E channel, uint8_t * txData, size_t length);
bool HW_SPI_receive(HW_SPI_channel_E channel, uint8_t * rxData, size_t length);
bool HW_SPI_transmitReceive(HW_SPI_channel_E channel, uint8_t * txData, uint8_t * rxData, size_t length);


#endif // HW_SPI_H
