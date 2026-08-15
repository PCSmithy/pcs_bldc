#pragma once

// Target-specific half of HW_I2C; reached via HW_I2C.h.

/* Includes */
#include "lib_types.h"
#include "stm32g4xx_hal.h"

#include "HW_I2C_channels.h"

/* Typedefs */

typedef struct
{
    bool enabled;
    I2C_HandleTypeDef hi2c;

    HW_I2C_transferMode_E transferMode;
    uint32_t sclBitRateHz;   // configured SCL bit rate; TIMINGR is opaque
} HW_I2C_busConfig_S;

/* Public Function Declarations */

// NVIC event / error ISR dispatch. The IRQ vectors (stm32g4xx_it.c) forward
// to these; each drives the HAL's I2C interrupt state machine for the bus.
void HW_I2C_irqHandlerEv(HW_I2C_bus_E bus);
void HW_I2C_irqHandlerEr(HW_I2C_bus_E bus);
