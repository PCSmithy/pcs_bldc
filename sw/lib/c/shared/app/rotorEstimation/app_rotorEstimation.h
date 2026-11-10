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
    float32_t inertia_kgm2;                 // rotor inertia J, > 0
    float32_t viscousFriction_NmPerRadPerSec;   // viscous friction b, >= 0: the torque opposing omega
    float32_t coulombFriction_Nm;           // Coulomb friction Tc, >= 0: the torque opposing omega through sat(omega / omega_db)
    float32_t disturbancePole_radPerSec;    // disturbance pole pd, > 0: the error dynamics' third pole
    float32_t coulombRampVelocity_radPerSec;   // Coulomb ramp omega_db, > 0: the speed at which the Coulomb term reaches +-Tc
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

// Mechanical angle [0, 2pi), velocity and unmodelled torque (Nm, positive
// in the sense of positive omega) estimates for the current cycle.
void app_rotorEstimation_getState(app_rotorEstimation_channel_E channel, float32_t * theta, float32_t * omega, float32_t * unmodeledTorque);

// The electromagnetic torque driving the rotor, used by the next cycle's advance.
void app_rotorEstimation_setTorqueInput(app_rotorEstimation_channel_E channel, float32_t torque_Nm);

#ifdef __cplusplus
}
#endif
