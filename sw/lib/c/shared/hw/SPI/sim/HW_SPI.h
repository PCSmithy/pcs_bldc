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
    char * channelNameStr;
} HW_SPI_channelConfig_S;

typedef struct
{
    const HW_SPI_channelConfig_S * channels;
    size_t numChannels;
} HW_SPI_config_S;

/* Static Inline Functions */

/* Public Function Declarations */

bool HW_SPI_init(const HW_SPI_config_S * const config);

#endif // HW_SPI_H
