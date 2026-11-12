#pragma once

// Test-local app_rgbLedRing seam: the board's one ring, its mode cycles counted.
typedef enum
{
    APP_RGBLEDRING_CHANNEL_RING,
    APP_RGBLEDRING_CHANNEL_COUNT,
} app_rgbLedRing_channel_E;

void app_rgbLedRing_cycleMode(app_rgbLedRing_channel_E channel);
