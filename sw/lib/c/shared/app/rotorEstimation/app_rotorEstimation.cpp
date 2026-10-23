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

using Mat12 = Mat<float32_t, 1, 2>;
using Mat21 = Mat<float32_t, 2, 1>;
using Mat22 = Mat<float32_t, 2, 2>;

// Second-order tracking observer on one rotor's raw mechanical angle:
// x = [theta, omega]. run1ms (task) measures and posts the correction K*e;
// run50us (ISR) applies it and advances x <- F x. The ISR is the only writer
// of the state, so the task's read of theta for the error is a single
// aligned float and the handoff needs no masking.
class Rotor
{
    public:
        bool init(const app_rotorEstimation_channelConfig_S * config);
        void run1ms();
        void run50us();

        void getState(float32_t * theta, float32_t * omega);

    private:
        app_rotorEstimation_channelConfig_S const * config;

        Mat21 K;   // correction gain per sample, [Kp*Ts, Ki*Ts]
        Mat22 F;   // one-cycle constant-velocity transition

        bool isSeeded;
        float32_t angleMeasured_rad;   // latest valid encoder angle
        float32_t angleError_rad;      // latest correction's e

        // The 1 ms correction, posted by run1ms as x <- x + correction and
        // applied by the next run50us. Posts are 1 ms apart and the ISR
        // applies within 50 us, so the task never rewrites one unconsumed.
        Mat21 correction;
        volatile uint32_t postSeq;
        uint32_t appliedSeq;

        Mat21 x_hat;   // state estimate, ISR-owned
};

typedef struct
{
    std::array<Rotor, APP_ROTORESTIMATION_CHANNEL_COUNT> channels{};
} app_rotorEstimation_data_S;

/* Private Data Definitions */
constinit static app_rotorEstimation_data_S app_rotorEstimation_data = {};
constinit static app_rotorEstimation_data_S * const data = &app_rotorEstimation_data;

static constexpr Mat12 H{{{1.0f, 0.0f}}};   // the measurement selects theta

/* Public Class Function Definitions */

bool Rotor::init(const app_rotorEstimation_channelConfig_S * channelConfig)
{
    bool isValid = (channelConfig != NULL);

    if (isValid)
    {
        const float32_t Ts = channelConfig->correctionPeriod_s;
        isValid &= (channelConfig->encoder < IO_AS5048_CHANNEL_COUNT);
        isValid &= (channelConfig->omega_n > ZERO);
        isValid &= (channelConfig->zeta >= 0.5f);
        isValid &= (Ts > ZERO);
        isValid &= ((channelConfig->estimatePeriod_s > ZERO) && (channelConfig->estimatePeriod_s <= Ts));
        // discrete-loop stability bound at any accepted damping
        isValid &= ((2.0f * channelConfig->zeta * channelConfig->omega_n * Ts) <= 1.0f);
    }

    if (isValid)
    {
        this->config = channelConfig;

        const float32_t Kp = 2.0f * channelConfig->zeta * channelConfig->omega_n;
        const float32_t Ki = channelConfig->omega_n * channelConfig->omega_n;
        this->K = Mat21{{{Kp * channelConfig->correctionPeriod_s}, {Ki * channelConfig->correctionPeriod_s}}};
        this->F = Mat22{{{1.0f, channelConfig->estimatePeriod_s}, {ZERO, 1.0f}}};

        this->isSeeded = false;
        this->angleMeasured_rad = ZERO;
        this->angleError_rad = ZERO;
        this->correction = Mat21{};
        this->postSeq = 0U;
        this->appliedSeq = 0U;
        this->x_hat = Mat21{};
    }
    return isValid;
}

// [impl->fw~est_rotor_001~1] the correction on a valid encoder read: the
// first lands the state on [theta_m, 0], every later one posts K*e against
// the current angle estimate.
void Rotor::run1ms()
{
    if (this->config != NULL)
    {
        IO_AS5048_status_E status = IO_AS5048_STATUS_IDLE;
        (void)IO_AS5048_getStatus(this->config->encoder, &status);

        float32_t theta_m = ZERO;
        const bool read = IO_AS5048_readAngle(this->config->encoder, NULL, NULL, &theta_m);

        if (read && (status == IO_AS5048_STATUS_OK))
        {
            this->angleMeasured_rad = theta_m;
            const Mat21 estimate = this->x_hat;

            if (this->isSeeded)
            {
                const float32_t e = WRAP_RAD_TO_PI(theta_m - (H * estimate).e[0][0]);
                this->angleError_rad = e;
                this->correction = this->K * e;
            }
            else
            {
                // the seed: the correction that moves the state onto [theta_m, 0]
                this->correction = Mat21{{{theta_m}, {ZERO}}} - estimate;
                this->isSeeded = true;
            }

            APP_ROTORESTIMATION_BARRIER_RELEASE();
            const uint32_t seq = this->postSeq;
            this->postSeq = seq + 1U;
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
        }

        this->x_hat = this->F * this->x_hat;
        this->x_hat.e[0][0] = lib_transforms_wrapAngle(this->x_hat.e[0][0]);
    }
}

void Rotor::getState(float32_t * theta, float32_t * omega)
{
    *theta = this->x_hat.e[0][0];
    *omega = this->x_hat.e[1][0];
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
void app_rotorEstimation_getState(app_rotorEstimation_channel_E channel, float32_t * theta, float32_t * omega)
{
    if (channel < APP_ROTORESTIMATION_CHANNEL_COUNT)
    {
        data->channels[channel].getState(theta, omega);
    }
}
