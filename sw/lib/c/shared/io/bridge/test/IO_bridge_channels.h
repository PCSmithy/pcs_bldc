#pragma once

// Test-local bridge-channel seam: the motor bridge, plus a second slot the
// multi-bridge init checks configure.
typedef enum
{
    IO_BRIDGE_CHANNEL_MOTOR,
    IO_BRIDGE_CHANNEL_SECOND,
    IO_BRIDGE_CHANNEL_COUNT,
} IO_bridge_channel_E;
