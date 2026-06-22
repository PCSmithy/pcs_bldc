#ifndef APP_RGBLEDRING_CHANNELS_H
#define APP_RGBLEDRING_CHANNELS_H

/* Typedefs */

// Logical ring-UI instances — one per RGB LED ring on the board.
typedef enum
{
    APP_RGBLEDRING_CHANNEL_RING,   // the 36-LED status ring
    APP_RGBLEDRING_CHANNEL_COUNT,
} app_rgbLedRing_channel_E;

#endif // APP_RGBLEDRING_CHANNELS_H
