#pragma once

// Test-local channel seam (mirrors the project-provided header). One ring is
// enough to size the module's per-channel state for the render-core tests.
typedef enum
{
    APP_RGBLEDRING_CHANNEL_RING,
    APP_RGBLEDRING_CHANNEL_COUNT,
} app_rgbLedRing_channel_E;

