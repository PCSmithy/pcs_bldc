#pragma once

// Test-local bridge-channel seam (real IO_bridge is linked; this supplies the
// channel enum its header needs). One motor bridge.
typedef enum
{
    IO_BRIDGE_CHANNEL_MOTOR,
    IO_BRIDGE_CHANNEL_COUNT,
} IO_bridge_channel_E;
