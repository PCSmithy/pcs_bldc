#pragma once

/* Includes */
#include "lib_types.h"

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

/* Target Config */
#include "HW_GPIO_target.h"   // HW_GPIO_portConfig_S

// One entry per HW_GPIO_port_E. Dense — index by HW_GPIO_PORT_x.
typedef struct
{
    HW_GPIO_portConfig_S ports[HW_GPIO_PORT_COUNT];
} HW_GPIO_config_S;

/* Public Function Declarations */

// Configure all ports listed in `config`. For each port with at least one pin,
// brings the port online and applies the initial output level of its
// output-mode pins (so they don't glitch out of reset state). Returns false on
// a NULL config or any per-port configuration failure.
bool HW_GPIO_init(const HW_GPIO_config_S * const config);

// Drive the configured output pins the mask touches to `level`. `pin` is a
// single-bit (or multi-bit) GPIO_PIN_x mask. No-op if `port` is out of range.
void HW_GPIO_writePin(HW_GPIO_port_E port, uint32_t pin, HW_GPIO_level_E level);

// Sample and cache every configured input pin's level. Call periodically
// (the 1 ms task) so consumers can fetch a recent, coherent snapshot via
// HW_GPIO_readCached without each one touching the hardware.
void HW_GPIO_run1ms(void);

// Return the cached level of input `pin` (single-bit GPIO_PIN_x mask) from
// the last HW_GPIO_run1ms() sample. HW_GPIO_LEVEL_LOW for an out-of-range
// port or a pin that isn't a configured input.
HW_GPIO_level_E HW_GPIO_readCached(HW_GPIO_port_E port, uint32_t pin);

// Register `callback` to fire once per configured signal edge on the
// interrupt-input `pin` (single-bit GPIO_PIN_x mask). `context` is passed
// back to the callback. Returns false on an out-of-range port.
bool HW_GPIO_registerExtiCallback(HW_GPIO_port_E port, uint32_t pin, HW_GPIO_extiCallback_F callback, void * context);
