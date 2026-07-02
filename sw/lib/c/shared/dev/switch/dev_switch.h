#ifndef DEV_SWITCH_H
#define DEV_SWITCH_H

/* Includes */
#include "lib_types.h"
#include "HW_GPIO.h"              // HW_GPIO_port_E, HW_GPIO_level_E
#include "dev_switch_channels.h"  // project-provided dev_switch_channel_E

/* Typedefs */

typedef enum
{
    DEV_SWITCH_STATE_UNKNOWN,
    DEV_SWITCH_STATE_INACTIVE,
    DEV_SWITCH_STATE_ACTIVE,
} dev_switch_state_E;

typedef enum
{
    DEV_SWITCH_TYPE_HW_DIGIN,
    DEV_SWITCH_TYPE_NETWORK,
    DEV_SWITCH_TYPE_COUNT,
} dev_switch_type_E;

typedef struct
{
    HW_GPIO_port_E  port;
    uint32_t        pin;           // single-bit GPIO_PIN_x mask
    HW_GPIO_level_E activeLevel;   // pin level that reads as "active" (pressed)
} dev_switch_hwDigInConfig_S;

typedef struct
{
    dev_switch_state_E (*getState)(void);
} dev_switch_networkConfig_S;

typedef struct
{
    dev_switch_type_E type;
    union
    {
        dev_switch_hwDigInConfig_S hwDigIn;
        dev_switch_networkConfig_S network;
    };

    uint16_t        debounce_ms;
} dev_switch_channelConfig_S;

typedef struct
{
    const dev_switch_channelConfig_S * channels;
    size_t numChannels;
} dev_switch_config_S;

/* Public Function Declarations */

bool dev_switch_init(const dev_switch_config_S * const config);

// Re-evaluate every channel's debounced state from the cached GPIO input.
// Call at 1 ms, after HW_GPIO_run1ms().
void dev_switch_run1ms(void);

dev_switch_state_E dev_switch_getState(dev_switch_channel_E channel);

// Debounced active (pressed) state of a channel. false for an out-of-range
// channel or before init.
bool dev_switch_isActive(dev_switch_channel_E channel);

#endif // DEV_SWITCH_H
