#pragma once

/* Includes */
#include "lib_types.h"


/* Typedefs */

struct PiConfig
{
    float32_t kp;
    float32_t ki;
    float32_t period_s;
};

// Discrete PI regulator v = kp * e + ki * S, then S <- S + period * e; after the
// caller limits v, backCalculate() sets S to match it and hold() undoes the step.
class PiRegulator
{
    public:
        [[nodiscard]] bool init(const PiConfig & config)
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

        // [impl->fw~mc_023~1] v = kp * e + ki * S, then S <- S + period * e. Zero before init.
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

        // [impl->fw~mc_024~1] S <- (vLimited - kp * e) / ki, so the next step at the
        // same error returns vLimited.
        void backCalculate(float32_t error, float32_t vLimited)
        {
            if (this->initialized)
            {
                this->integral = (vLimited - this->config.kp * error) / this->config.ki;
            }
        }

        // [impl->fw~mc_029~1] S <- S - period * e: the step just taken at this error
        // left out of the integral, which holds through a clamp.
        void hold(float32_t error)
        {
            if (this->initialized)
            {
                this->integral = this->integral - this->config.period_s * error;
            }
        }

    private:
        // Trivial on purpose: a zero object is an uninitialized regulator, so a
        // constinit holder needs no constructor. Value-initialize locals: `PiRegulator r = {};`
        PiConfig  config;
        bool      initialized;
        float32_t integral;   // the integral of the error, S
};
