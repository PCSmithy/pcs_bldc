#ifndef HW_SPI_CHANNELS_H
#define HW_SPI_CHANNELS_H

// Test-local SPI channel seam. The mock HW_SPI captures the transmitted
// buffer keyed by these.
typedef enum
{
    HW_SPI_CHANNEL_LED_A,
    HW_SPI_CHANNEL_LED_B,
    HW_SPI_CHANNEL_COUNT,
} HW_SPI_channel_E;

#endif // HW_SPI_CHANNELS_H
