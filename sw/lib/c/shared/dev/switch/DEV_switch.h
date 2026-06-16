#ifndef DEV_SWITCH_H
#define DEV_SWITCH_H

/* Includes */
#include "lib_types.h"
#include "HW_GPIO.h"              // HW_GPIO_port_E, HW_GPIO_level_E
#include "DEV_switch_channels.h"  // project-provided DEV_switch_channel_E

/* Typedefs */

typedef struct
{
    HW_GPIO_port_E  port;
    uint32_t        pin;           // single-bit GPIO_PIN_x mask
    HW_GPIO_level_E activeLevel;   // pin level that reads as "active" (pressed)
    uint16_t        debounceCount; // consecutive 1 ms samples to accept a change
} DEV_switch_channelConfig_S;

typedef struct
{
    const DEV_switch_channelConfig_S * channels;
    size_t numChannels;
} DEV_switch_config_S;

/* Public Function Declarations */

bool DEV_switch_init(const DEV_switch_config_S * const config);

// Re-evaluate every channel's debounced state from the cached GPIO input.
// Call at 1 ms, after HW_GPIO_run1ms().
void DEV_switch_run1ms(void);

// Debounced active (pressed) state of a channel. false for an out-of-range
// channel or before init.
bool DEV_switch_isActive(DEV_switch_channel_E channel);

#endif // DEV_SWITCH_H
