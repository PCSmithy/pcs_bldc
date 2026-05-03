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

// Mirror of the stm32g4 struct shape, but with no HAL types — pin
// configs are just human-readable names for trace logs. SIL doesn't
// have real pins to configure; this layer exists to keep the API
// surface uniform across targets.
typedef struct
{
    const char * pinNameStr;
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

#endif // HW_GPIO_H
