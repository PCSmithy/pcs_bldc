#ifndef HW_GPIO_H
#define HW_GPIO_H

/* Includes */
#include "lib_types.h"

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

typedef enum
{
    HW_GPIO_MODE_INPUT,
    HW_GPIO_MODE_OUTPUT,
    HW_GPIO_MODE_INTERRUPT,
} HW_GPIO_mode_E;

// Mirror of the stm32g4 struct shape, but with no HAL types — pin
// configs are just human-readable names for trace logs. SIL doesn't
// have real pins to configure; this layer exists to keep the API
// surface uniform across targets.
typedef struct
{
    uint32_t       pin;        // single-bit (or multi-bit) GPIO_PIN_x mask
    HW_GPIO_mode_E mode;
    const char *   pinNameStr; // human-readable name for trace logs
} HW_GPIO_pinConfig_S;

typedef struct
{
    const HW_GPIO_pinConfig_S * pins;
    size_t numPins;
} HW_GPIO_portConfig_S;

typedef struct
{
    HW_GPIO_portConfig_S ports[HW_GPIO_PORT_COUNT];
} HW_GPIO_config_S;

/* Static Inline Functions */

/* Public Function Declarations */
bool HW_GPIO_init(const HW_GPIO_config_S * const config);

// Records the write for SIL inspection (see HW_GPIO_sim.h). `pin` is a
// single-bit mask matching the stm32g4 GPIO_PIN_x encoding.
void HW_GPIO_writePin(HW_GPIO_port_E port, uint32_t pin, HW_GPIO_level_E level);

// Sample and cache every configured input pin's level (from the injected SIL
// input state). Call periodically so consumers can fetch a recent snapshot
// via HW_GPIO_readCached.
void HW_GPIO_run1ms(void);

// Return the cached level of input `pin` (single-bit GPIO_PIN_x mask) from
// the last HW_GPIO_run1ms() sample. HW_GPIO_LEVEL_LOW for an out-of-range
// port or a pin that isn't a configured input.
HW_GPIO_level_E HW_GPIO_readCached(HW_GPIO_port_E port, uint32_t pin);

// Register `callback` to fire once per configured signal edge on the
// interrupt-input `pin` (single-bit GPIO_PIN_x mask). `context` is passed
// back to the callback. Returns false on an out-of-range port.
bool HW_GPIO_registerExtiCallback(HW_GPIO_port_E port, uint32_t pin, HW_GPIO_extiCallback_F callback, void * context);

#endif // HW_GPIO_H
