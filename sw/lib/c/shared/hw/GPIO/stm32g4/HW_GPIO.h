#ifndef HW_GPIO_H
#define HW_GPIO_H

/* Includes */
#include "lib_types.h"
#include "stm32g4xx_hal.h"

/* Defines */

/* Typedefs */
typedef enum
{
    HW_GPIO_PORT_A,
    HW_GPIO_PORT_B,
    HW_GPIO_PORT_C,
    HW_GPIO_PORT_D,
    HW_GPIO_PORT_E,
    HW_GPIO_PORT_F,
    HW_GPIO_PORT_G,

    HW_GPIO_PORT_COUNT,
} HW_GPIO_port_E;

typedef enum
{
    HW_GPIO_LEVEL_LOW,
    HW_GPIO_LEVEL_HIGH,
} HW_GPIO_level_E;

typedef void (*HW_GPIO_extiCallback_F)(HW_GPIO_port_E port, uint32_t pin, void * context);

typedef struct
{
    const GPIO_InitTypeDef * pins;
    size_t numPins;
} HW_GPIO_portConfig_S;

// One entry per HW_GPIO_port_E. Dense — index by HW_GPIO_PORT_x.
typedef struct
{
    HW_GPIO_portConfig_S ports[HW_GPIO_PORT_COUNT];
} HW_GPIO_config_S;

/* Static Inline Functions */

/* Public Function Declarations */

// Configure all ports listed in `config`. For each port with at least
// one pin, enables the port's RCC clock, sets the initial output level
// for any output-mode pins (so they don't glitch when switched from
// reset state to output mode), and calls HAL_GPIO_Init. Returns false
// on NULL config or any HAL_GPIO_Init failure.
bool HW_GPIO_init(const HW_GPIO_config_S * const config);

// Drive a single configured output pin to `level`. `pin` is a HAL pin
// mask (GPIO_PIN_x). No-op if `port` is out of range.
void HW_GPIO_writePin(HW_GPIO_port_E port, uint32_t pin, HW_GPIO_level_E level);

// Read the present logical level of a configured input pin. `pin` is a
// single-bit GPIO_PIN_x mask. Returns HW_GPIO_LEVEL_LOW for an out-of-range port.
HW_GPIO_level_E HW_GPIO_readPin(HW_GPIO_port_E port, uint32_t pin);

// Register `callback` to fire once per configured signal edge on the
// interrupt-input `pin` (single-bit GPIO_PIN_x mask). `context` is passed
// back to the callback. Returns false on an out-of-range port.
bool HW_GPIO_registerExtiCallback(HW_GPIO_port_E port, uint32_t pin, HW_GPIO_extiCallback_F callback, void * context);

#endif // HW_GPIO_H
