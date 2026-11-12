#pragma once

// Test-local dev_switch seam: the board's one button, its level set by the test.
#include "lib_types.h"

typedef enum
{
    DEV_SWITCH_CHANNEL_USER_BUTTON,
    DEV_SWITCH_CHANNEL_COUNT,
} dev_switch_channel_E;

bool dev_switch_isActive(dev_switch_channel_E channel);
