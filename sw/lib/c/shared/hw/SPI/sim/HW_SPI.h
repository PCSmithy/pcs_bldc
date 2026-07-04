#pragma once

/* Includes */
#include "lib_types.h"

#include "HW_GPIO.h"
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
    HW_SPI_transferMode_E transferMode;
    char * busNameStr;
} HW_SPI_busConfig_S;

typedef enum
{
    HW_SPI_CS_MODE_NONE,
    HW_SPI_CS_MODE_HW,
    HW_SPI_CS_MODE_GPIO,
} HW_SPI_chipSelectMode_E;

typedef struct
{
    HW_GPIO_port_E port;
    uint32_t pin;
    HW_GPIO_level_E activeLevel; // level driven to assert (select) the device
} HW_SPI_csGpioConfig_S;

typedef struct
{
    HW_SPI_bus_E bus;
    HW_SPI_chipSelectMode_E csMode;
    HW_SPI_csGpioConfig_S csGpioConfig; // ignored if csMode != GPIO
    char * channelNameStr;
} HW_SPI_channelConfig_S;

typedef struct
{
    const HW_SPI_busConfig_S * buses;
    size_t numBuses;

    const HW_SPI_channelConfig_S * channels;
    size_t numChannels;
} HW_SPI_config_S;

// Per-channel transfer status. Software transfers leave the channel
// IDLE->COMPLETE/ERROR synchronously; non-blocking transfers pass
// through BUSY until completion is signalled.
typedef enum
{
    HW_SPI_STATUS_IDLE,
    HW_SPI_STATUS_BUSY,
    HW_SPI_STATUS_COMPLETE,
    HW_SPI_STATUS_ERROR,
} HW_SPI_status_E;

// Invoked once at completion of a non-blocking transfer. `context` is
// the pointer supplied to HW_SPI_registerCallback.
typedef void (*HW_SPI_completeCallback_F)(HW_SPI_channel_E channel, void * context);

/* Static Inline Functions */

/* Public Function Declarations */

bool HW_SPI_init(const HW_SPI_config_S * const config);

bool HW_SPI_transmit(HW_SPI_channel_E channel, uint8_t * txData, size_t length);
bool HW_SPI_receive(HW_SPI_channel_E channel, uint8_t * rxData, size_t length);
bool HW_SPI_transmitReceive(HW_SPI_channel_E channel, uint8_t * txData, uint8_t * rxData, size_t length);

bool HW_SPI_registerCallback(HW_SPI_channel_E channel, HW_SPI_completeCallback_F callback, void * context);
HW_SPI_status_E HW_SPI_getStatus(HW_SPI_channel_E channel);

