#pragma once

// Test-local SPI channel seam. The mock HW_SPI keys its per-channel
// injected responses on these.
typedef enum
{
    HW_SPI_CHANNEL_ENC_A,
    HW_SPI_CHANNEL_ENC_B,
    HW_SPI_CHANNEL_COUNT,
} HW_SPI_channel_E;

