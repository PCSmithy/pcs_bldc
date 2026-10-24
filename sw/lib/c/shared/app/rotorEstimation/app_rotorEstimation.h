#pragma once

/* Includes */
#include "lib_types.h"

#include "IO_AS5048_channels.h"

#include "app_rotorEstimation_channels.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Defines */


/* Typedefs */

typedef struct
{
    IO_AS5048_channel_E encoder;

    float32_t naturalFrequency_radPerSec;   // observer natural frequency omega_n
    float32_t dampingRatio;                 // observer damping ratio zeta, >= 0.5
    float32_t correctionPeriod_s;           // the encoder sample period (run1ms)
    float32_t estimatePeriod_s;             // the PWM period (run50us)
} app_rotorEstimation_channelConfig_S;

typedef struct
{
    const app_rotorEstimation_channelConfig_S * channels;
    size_t numChannels;
} app_rotorEstimation_config_S;


/* Public Function Declarations */

bool app_rotorEstimation_init(const app_rotorEstimation_config_S * const config);

void app_rotorEstimation_run1ms(void);
void app_rotorEstimation_run50us(void); // called from 20kHz ISR

// Mechanical angle [0, 2pi) and velocity estimates for the current cycle.
void app_rotorEstimation_getState(app_rotorEstimation_channel_E channel, float32_t * theta, float32_t * omega);

#ifdef __cplusplus
}
#endif
