/* Includes */
#include "lib_utils.h"
#include "lib_transforms.h"
#include "lib_matrix.hpp"

#include "IO_AS5048.h"
#include "app_rotorEstimation.h"
#include "app_rotorEstimation_channels.h"

#include <array>

/* Defines */
#define ZERO (0.0f)

// Task -> ISR ordering of the posted correction against its sequence number.
#define APP_ROTORESTIMATION_BARRIER_ACQUIRE() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define APP_ROTORESTIMATION_BARRIER_RELEASE() __atomic_thread_fence(__ATOMIC_RELEASE)

/* Typedefs */

using Mat31 = Mat<float32_t, 3, 1>;
using Mat33 = Mat<float32_t, 3, 3>;

// Third-order tracking observer on one rotor's raw mechanical angle:
// x = [theta, omega, T_d], T_d the torque on the rotor beyond the model's.
// run1ms (task) measures and posts the correction K*e; run50us (ISR)
// applies it and advances x <- F x + inputGain * modelledTorque, from the
// input motor control sets. The ISR is the only writer of the state; the
// task reads each element as one aligned 32-bit load.
class Rotor
{
    public:
        bool init(const app_rotorEstimation_channelConfig_S * const channelConfig);
        void run1ms();
        void run50us();

        void getState(float32_t * const theta, float32_t * const omega, float32_t * const unmodeledTorque);
        void setTorqueInput(float32_t torque_Nm);

    private:
        app_rotorEstimation_channelConfig_S const * config;

        Mat31 K;   // correction gain per sample

        Mat33 F;           // one-cycle state transition
        Mat31 inputGain;   // one-cycle input transition
        float32_t coulombRampGain;   // 1 / omega_db: the Coulomb term's slope through zero

        bool isSeeded;                 // set by the ISR once a seed has landed
        bool seedPosted;               // task: the latest post is a seed
        uint32_t invalidReads;         // task: consecutive failed or invalid reads, saturating
        float32_t angleMeasured_rad;   // latest valid encoder angle
        float32_t angleError_rad;      // latest correction's e

        // The 1 ms correction, posted by run1ms as x <- x + correction and
        // applied by the next run50us. Posts are 1 ms apart and the ISR
        // applies within 50 us; a post the ISR has not consumed by the next
        // read is overwritten by the newer one, which is the right one for
        // the unchanged state. A seed is re-posted until it lands.
        Mat31 correction;
        volatile uint32_t postSeq;
        uint32_t appliedSeq;

        Mat31 x_hat;                   // state estimate, ISR-owned
        float32_t torqueInput_Nm;      // set by motor control each cycle, after the advance
        float32_t modelledTorque_Nm;   // the advance's input, torque - b omega - Tc sat(omega / omega_db)
};

typedef struct
{
    std::array<Rotor, APP_ROTORESTIMATION_CHANNEL_COUNT> channels{};
} app_rotorEstimation_data_S;

/* Private Data Definitions */
constinit static app_rotorEstimation_data_S app_rotorEstimation_data = {};
constinit static app_rotorEstimation_data_S * const data = &app_rotorEstimation_data;

/* Public Class Function Definitions */

bool Rotor::init(const app_rotorEstimation_channelConfig_S * const channelConfig)
{
    bool isValid = (channelConfig != NULL);

    if (isValid)
    {
        const float32_t correctionPeriod_s = channelConfig->correctionPeriod_s;
        isValid &= (channelConfig->encoder < IO_AS5048_CHANNEL_COUNT);
        isValid &= (channelConfig->naturalFrequency_radPerSec > ZERO);
        isValid &= (channelConfig->dampingRatio >= 0.5f);
        isValid &= (channelConfig->disturbancePole_radPerSec > ZERO);
        isValid &= (correctionPeriod_s > ZERO);
        isValid &= ((channelConfig->estimatePeriod_s > ZERO) && (channelConfig->estimatePeriod_s <= correctionPeriod_s));
        // [impl->fw~est_rotor_002~1] J and omega_db are divisors; friction opposes motion
        isValid &= (channelConfig->inertia_kgm2 > ZERO);
        isValid &= (channelConfig->viscousFriction_NmPerRadPerSec >= ZERO);
        isValid &= (channelConfig->coulombFriction_Nm >= ZERO);
        isValid &= (channelConfig->coulombRampVelocity_radPerSec > ZERO);
        // discrete-loop stability bound on the error poles' summed rates
        isValid &= ((((2.0f * channelConfig->dampingRatio * channelConfig->naturalFrequency_radPerSec) +
                     channelConfig->disturbancePole_radPerSec) * correctionPeriod_s) <= 1.0f);
    }

    if (isValid)
    {
        this->config = channelConfig;

        const float32_t naturalFrequency_radPerSec = channelConfig->naturalFrequency_radPerSec;
        const float32_t dampingRatio = channelConfig->dampingRatio;
        const float32_t disturbancePole_radPerSec = channelConfig->disturbancePole_radPerSec;
        const float32_t inertia_kgm2 = channelConfig->inertia_kgm2;
        const float32_t frictionRate_perSec = channelConfig->viscousFriction_NmPerRadPerSec / inertia_kgm2;
        const float32_t correctionPeriod_s = channelConfig->correctionPeriod_s;
        const float32_t period_s = channelConfig->estimatePeriod_s;

        // l1, l2, l3: error poles at the roots of (s^2 + 2 zeta wn s + wn^2)(s + pd)
        const float32_t angleGain_perSec = (2.0f * dampingRatio * naturalFrequency_radPerSec) +
                                           disturbancePole_radPerSec - frictionRate_perSec;
        const float32_t velocityGain_perSec2 = (naturalFrequency_radPerSec * naturalFrequency_radPerSec) +
                                               (2.0f * dampingRatio * naturalFrequency_radPerSec * disturbancePole_radPerSec) -
                                               (angleGain_perSec * frictionRate_perSec);
        const float32_t torqueGain_NmPerRadSec = inertia_kgm2 * naturalFrequency_radPerSec *
                                                 naturalFrequency_radPerSec * disturbancePole_radPerSec;
        this->K = Mat31{{{angleGain_perSec * correctionPeriod_s},
                         {velocityGain_perSec2 * correctionPeriod_s},
                         {torqueGain_NmPerRadSec * correctionPeriod_s}}};

        // the kinematic advance, T_d acting through 1/J; the input carries the modelled torque
        const float32_t angleInputGain = (period_s * period_s) / (2.0f * inertia_kgm2);
        const float32_t velocityInputGain = period_s / inertia_kgm2;
        this->F = Mat33{{{1.0f, period_s, angleInputGain}, {ZERO, 1.0f, velocityInputGain}, {ZERO, ZERO, 1.0f}}};
        this->inputGain = Mat31{{{angleInputGain}, {velocityInputGain}, {ZERO}}};
        this->coulombRampGain = 1.0f / channelConfig->coulombRampVelocity_radPerSec;

        this->isSeeded = false;
        this->seedPosted = false;
        this->invalidReads = 0U;
        this->angleMeasured_rad = ZERO;
        this->angleError_rad = ZERO;
        this->correction = Mat31{};
        this->postSeq = 0U;
        this->appliedSeq = 0U;
        this->x_hat = Mat31{};
        this->torqueInput_Nm = ZERO;
        this->modelledTorque_Nm = ZERO;
    }
    return isValid;
}

// [impl->fw~est_rotor_001~1] the correction on a valid encoder read: a seed onto
// [theta_m, 0, 0] until one lands and again after more than reseedAfterInvalidReads
// failed reads in a row; otherwise K*e against the angle estimate (H = [1 0 0]).
void Rotor::run1ms()
{
    if (this->config != NULL)
    {
        IO_AS5048_status_E status = IO_AS5048_STATUS_IDLE;
        (void)IO_AS5048_getStatus(this->config->encoder, &status);

        float32_t angle_rad = ZERO;
        const bool read = IO_AS5048_readAngle(this->config->encoder, NULL, NULL, &angle_rad);

        if (read && (status == IO_AS5048_STATUS_OK))
        {
            this->angleMeasured_rad = angle_rad;

            uint32_t appliedSeq = 0U;
            __atomic_load(&this->appliedSeq, &appliedSeq, __ATOMIC_RELAXED);
            const bool seedOutstanding = (this->seedPosted && (appliedSeq != this->postSeq));
            const bool seed = ((!this->isSeeded) || seedOutstanding ||
                               (this->invalidReads > this->config->reseedAfterInvalidReads));

            float32_t thetaHat = ZERO;
            __atomic_load(&this->x_hat.e[0][0], &thetaHat, __ATOMIC_RELAXED);

            if (seed)
            {
                float32_t omegaHat = ZERO;
                float32_t torqueHat = ZERO;
                __atomic_load(&this->x_hat.e[1][0], &omegaHat, __ATOMIC_RELAXED);
                __atomic_load(&this->x_hat.e[2][0], &torqueHat, __ATOMIC_RELAXED);
                this->correction = Mat31{{{angle_rad - thetaHat}, {-omegaHat}, {-torqueHat}}};
            }
            else
            {
                const float32_t error_rad = WRAP_RAD_TO_PI(angle_rad - thetaHat);
                this->angleError_rad = error_rad;
                this->correction = this->K * error_rad;
            }
            this->seedPosted = seed;
            this->invalidReads = 0U;

            APP_ROTORESTIMATION_BARRIER_RELEASE();
            const uint32_t seq = this->postSeq;
            this->postSeq = seq + 1U;
        }
        else if (this->invalidReads < UINT32_MAX)
        {
            this->invalidReads++;
        }
        else
        {
            // saturated
        }
    }
}

// [impl->fw~est_rotor_002~1] the per-cycle advance in the bridge callback's
// ISR context, after applying any correction run1ms posted.
void Rotor::run50us()
{
    if (this->config != NULL)
    {
        const uint32_t seq = this->postSeq;
        if (seq != this->appliedSeq)
        {
            APP_ROTORESTIMATION_BARRIER_ACQUIRE();
            this->x_hat = this->x_hat + this->correction;
            this->appliedSeq = seq;
            this->isSeeded = true;
        }

        const float32_t omegaHat = this->x_hat.e[1][0];
        this->modelledTorque_Nm = this->torqueInput_Nm - (this->config->viscousFriction_NmPerRadPerSec * omegaHat) -
                                  (this->config->coulombFriction_Nm * SATURATE((omegaHat * this->coulombRampGain), -1.0f, 1.0f));
        this->x_hat = (this->F * this->x_hat) + (this->inputGain * this->modelledTorque_Nm);
        this->x_hat.e[0][0] = lib_transforms_wrapAngle(this->x_hat.e[0][0]);
    }
}

// Readable before init too: an unconfigured channel reads zeros.
void Rotor::getState(float32_t * const theta, float32_t * const omega, float32_t * const unmodeledTorque)
{
    __atomic_load(&this->x_hat.e[0][0], theta, __ATOMIC_RELAXED);
    __atomic_load(&this->x_hat.e[1][0], omega, __ATOMIC_RELAXED);
    __atomic_load(&this->x_hat.e[2][0], unmodeledTorque, __ATOMIC_RELAXED);
}

void Rotor::setTorqueInput(float32_t torque_Nm)
{
    this->torqueInput_Nm = torque_Nm;
}

/* Public Function Definitions */

bool app_rotorEstimation_init(const app_rotorEstimation_config_S * const config)
{
    bool success = false;
    if ((config != NULL) && (config->channels != NULL) && (config->numChannels <= APP_ROTORESTIMATION_CHANNEL_COUNT))
    {
        bool configsValid = true;
        for (size_t channel = 0U; channel < config->numChannels; channel++)
        {
            configsValid &= data->channels[channel].init(&config->channels[channel]);
        }
        success = configsValid;
    }
    return success;
}

void app_rotorEstimation_run1ms(void)
{
    for (size_t channel = 0U; channel < APP_ROTORESTIMATION_CHANNEL_COUNT; channel++)
    {
        data->channels[channel].run1ms();
    }
}

void app_rotorEstimation_run50us(void)
{
    for (size_t channel = 0U; channel < APP_ROTORESTIMATION_CHANNEL_COUNT; channel++)
    {
        data->channels[channel].run50us();
    }
}

// Hot path (20 kHz): the out-pointers are not checked.
void app_rotorEstimation_getState(app_rotorEstimation_channel_E channel,
                                  float32_t * const theta,
                                  float32_t * const omega,
                                  float32_t * const unmodeledTorque)
{
    if (channel < APP_ROTORESTIMATION_CHANNEL_COUNT)
    {
        data->channels[channel].getState(theta, omega, unmodeledTorque);
    }
}

void app_rotorEstimation_setTorqueInput(app_rotorEstimation_channel_E channel, float32_t torque_Nm)
{
    if (channel < APP_ROTORESTIMATION_CHANNEL_COUNT)
    {
        data->channels[channel].setTorqueInput(torque_Nm);
    }
}
