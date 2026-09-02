#pragma once

// Sim-internal driver state. Included by HW_GPIO.c and its Unity suite only —
// it is the same object SIL reaches by DWARF, so injection and observation stay
// white-box on both surfaces and the driver carries no test-only API.

/* Includes */
#include "HW_GPIO.h"

/* Defines */

#define HW_GPIO_SIM_PINS_PER_PORT    (16U)

/* Typedefs */

typedef struct
{
    const HW_GPIO_config_S * config;
    bool initialized;

    // Per-pin injected input level and EXTI registration. SIL injects inputs by
    // writing inputLevel directly (DWARF).
    HW_GPIO_level_E        inputLevel[HW_GPIO_PORT_COUNT][HW_GPIO_SIM_PINS_PER_PORT];
    HW_GPIO_extiCallback_F extiCallback[HW_GPIO_PORT_COUNT][HW_GPIO_SIM_PINS_PER_PORT];
    void *                 extiContext[HW_GPIO_PORT_COUNT][HW_GPIO_SIM_PINS_PER_PORT];

    // SIL output-port handles (SIL_PORTS_HANDLE_INVALID when unregistered),
    // indexed by port then by the pin's index in that port's config array.
    int32_t outputHandle[HW_GPIO_PORT_COUNT][HW_GPIO_SIM_PINS_PER_PORT];

    // Polled-input cache (mirror of the stm32g4 driver): inputMask marks the
    // configured GPIO_MODE_INPUT pins, cachedInput holds their last sample.
    uint16_t inputMask[HW_GPIO_PORT_COUNT];
    uint16_t cachedInput[HW_GPIO_PORT_COUNT];

    // EXTI edge detector: interruptMask marks every configured interrupt pin,
    // risingMask/fallingMask which of its edges that pin's trigger type accepts.
    // extiEdgeCount counts dispatched edges per port — SIL reads it by DWARF.
    uint16_t interruptMask[HW_GPIO_PORT_COUNT];
    uint16_t risingMask[HW_GPIO_PORT_COUNT];
    uint16_t fallingMask[HW_GPIO_PORT_COUNT];
    uint16_t lastInterruptLevel[HW_GPIO_PORT_COUNT];
    uint32_t extiEdgeCount[HW_GPIO_PORT_COUNT];
} HW_GPIO_data_S;

/* Public Data Declarations */

extern HW_GPIO_data_S HW_GPIO_data;
