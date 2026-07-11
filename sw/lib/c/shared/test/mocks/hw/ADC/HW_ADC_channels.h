#pragma once

// Test-local HW_ADC channel seam: two ADC peripherals, matching the board's
// ADC1/ADC2 split so the current-sense config can route phases across both.
typedef enum
{
    HW_ADC_CHANNEL_1,
    HW_ADC_CHANNEL_2,
    HW_ADC_CHANNEL_COUNT,
} HW_ADC_channels_E;
