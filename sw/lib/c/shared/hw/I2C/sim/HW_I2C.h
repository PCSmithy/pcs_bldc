#pragma once

/* Includes */
#include "lib_types.h"

#include "HW_I2C_channels.h"

/* Defines */

/* Typedefs */

typedef enum
{
    HW_I2C_TRANSFERMODE_INTERRUPT,
    HW_I2C_TRANSFERMODE_DMA,
} HW_I2C_transferMode_E;

// Register-offset encoding for HW_I2C_memRead / HW_I2C_memWrite. 16BIT sends
// the offset MSB-first (the I2C EEPROM convention); 16BIT_LSBFIRST sends it
// LSB-first for devices like the Cypress HPI that expect the low byte first.
typedef enum
{
    HW_I2C_MEMADDR_SIZE_8BIT,
    HW_I2C_MEMADDR_SIZE_16BIT,
    HW_I2C_MEMADDR_SIZE_16BIT_LSBFIRST,
} HW_I2C_memAddrSize_E;

typedef struct
{
    bool enabled;
    HW_I2C_transferMode_E transferMode;
    uint32_t sclBitRateHz;
    char * busNameStr;
} HW_I2C_busConfig_S;

typedef struct
{
    const HW_I2C_busConfig_S * buses;
    size_t numBuses;
} HW_I2C_config_S;

/* Static Inline Functions */

/* Public Function Declarations */

bool HW_I2C_init(const HW_I2C_config_S * const config);

bool HW_I2C_transmit(HW_I2C_bus_E bus, uint8_t devAddr7, uint8_t * data, size_t length);
bool HW_I2C_receive(HW_I2C_bus_E bus, uint8_t devAddr7, uint8_t * data, size_t length);

bool HW_I2C_memRead(HW_I2C_bus_E bus, uint8_t devAddr7, uint16_t memAddr,
                    HW_I2C_memAddrSize_E memAddrSize, uint8_t * data, size_t length);
bool HW_I2C_memWrite(HW_I2C_bus_E bus, uint8_t devAddr7, uint16_t memAddr,
                     HW_I2C_memAddrSize_E memAddrSize, uint8_t * data, size_t length);
