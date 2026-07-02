#ifndef HW_DMA_CHANNELS_H
#define HW_DMA_CHANNELS_H

/* Typedefs */

// Logical DMA channels. SK6805_TX streams the LED frame (memory -> SPI3);
// AS5048_RX reads the encoder (SPI1 -> memory). Consumed by HW_SPI (M4) to make
// those transfers non-blocking.
typedef enum
{
    HW_DMA_CHANNEL_SK6805_TX,
    HW_DMA_CHANNEL_AS5048_RX,
    HW_DMA_CHANNEL_COUNT,
} HW_DMA_channel_E;

#endif // HW_DMA_CHANNELS_H
