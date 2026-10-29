#pragma once

/* Includes */
#include "lib_types.h"
#include "lib_utils.h"


/* Defines */

/* Typedefs */

// Discrete PI regulator, fw~mc_023's form: v = kp * e + ki * S, then
// S <- S + period * e. backCalculate() is fw~mc_024's anti-windup: after the
// caller limits v, S is set so kp * e + ki * S equals the limited v.
struct PiConfig
{
    float32_t kp;
    float32_t ki;
    float32_t period_s;
};

class PiRegulator
{
    public:
        bool init(const PiConfig & config)
        {
            const bool isValid = ((config.ki != 0.0f) &&
                                  (config.period_s > 0.0f));
            if (isValid)
            {
                this->config = config;
                this->integral = 0.0f;
                this->initialized = true;
            }
            return isValid;
        }

        // Zero the integral.
        void reset()
        {
            if (this->initialized)
            {
                this->integral = 0.0f;
            }
        }

        // v = kp * e + ki * S, then S <- S + period * e. Zero before init.
        [[nodiscard]] float32_t step(float32_t error)
        {
            float32_t v = 0.0f;
            if (this->initialized)
            {
                v = this->config.kp * error + this->config.ki * this->integral;
                this->integral = this->integral + this->config.period_s * error;
            }
            return v;
        }

        // S <- (vLimited - kp * e) / ki, so the next step at the same error
        // returns vLimited.
        void backCalculate(float32_t error, float32_t vLimited)
        {
            if (this->initialized)
            {
                this->integral = (vLimited - this->config.kp * error) / this->config.ki;
            }
        }

    private:
        // Trivial on purpose: a zero object is an uninitialized regulator, so a
        // constinit holder needs no constructor. Value-initialize locals: `PiRegulator r = {};`
        PiConfig  config;
        bool      initialized;
        float32_t integral;   // S
};
