#ifndef DEV_SWITCH_H
#define DEV_SWITCH_H

// Minimal mock of the dev_switch surface app_rgbLedRing uses (channel enum +
// debounced state read). Implemented as a stub in mock_app_deps.c.
#include "lib_types.h"

typedef enum
{
    DEV_SWITCH_CHANNEL_USER_BUTTON,
    DEV_SWITCH_CHANNEL_COUNT,
} dev_switch_channel_E;

bool dev_switch_isActive(dev_switch_channel_E channel);

#endif // DEV_SWITCH_H
