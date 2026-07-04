#pragma once

/// INCLUDES

#include "lib_types.h"

/// DEFINES

// 7-bit I2C address of the CYPD3177 HPI interface.
#define CYPD3177_I2C_ADDR_7BIT (0x08U)

// CURRENT_PDO fixed-supply voltage field: bits [19:10], 50 mV per LSB.
#define CYPD3177_PDO_VOLTAGE_SHIFT    (10U)
#define CYPD3177_PDO_VOLTAGE_MASK     (0x3FFU)
#define CYPD3177_PDO_VOLTAGE_SCALE_MV (50U)

// CURRENT_RDO operating-current field: bits [19:10], 10 mA per LSB.
#define CYPD3177_RDO_CURRENT_SHIFT    (10U)
#define CYPD3177_RDO_CURRENT_MASK     (0x3FFU)
#define CYPD3177_RDO_CURRENT_SCALE_MA (10U)

// PD_STATUS explicit-contract flag: bit 10.
#define CYPD3177_PD_STATUS_CONTRACT_SHIFT (10U)
#define CYPD3177_PD_STATUS_CONTRACT_MASK  (0x1U)

// BUS_VOLTAGE byte: 100 mV per LSB.
#define CYPD3177_BUS_VOLTAGE_SCALE_MV (100U)

/// TYPEDEFS

// HPI register offsets (16-bit) addressed over I2C.
typedef enum {
    CYPD3177_REG_DEVICE_MODE   = 0x0000,
    CYPD3177_REG_SILICON_ID    = 0x0002,
    CYPD3177_REG_PD_STATUS     = 0x1008,
    CYPD3177_REG_TYPE_C_STATUS = 0x100C,
    CYPD3177_REG_BUS_VOLTAGE   = 0x100D,
    CYPD3177_REG_CURRENT_PDO   = 0x1010,
    CYPD3177_REG_CURRENT_RDO   = 0x1014,
} CYPD3177_register_E;

/// PUBLIC FUNCTIONS

// ---------- Accessors ----------
bool     CYPD3177_isContractActive    (uint32_t pdStatus);
uint32_t CYPD3177_negotiatedVoltage_mV(uint32_t currentPdo);
uint32_t CYPD3177_negotiatedCurrent_mA(uint32_t currentRdo);
uint32_t CYPD3177_busVoltage_mV       (uint8_t  busVoltage);
