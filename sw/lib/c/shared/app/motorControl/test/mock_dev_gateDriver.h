#pragma once

#include "lib_types.h"
#include "dev_gateDriver_channels.h"

// Controls for the dev_gateDriver boundary double.

// Clear all channels to not-operational (matches the real driver before it is
// configured).
void mock_dev_gateDriver_reset(void);

// Set whether `channel` reports operational.
void mock_dev_gateDriver_setOperational(dev_gateDriver_channel_E channel, bool operational);
