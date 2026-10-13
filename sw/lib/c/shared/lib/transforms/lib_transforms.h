#pragma once

/* Includes */
#include "lib_types.h"
#include "lib_utils.h"
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif


/* Defines */

#define LIB_TRANSFORMS_SQRT3 (1.7320508f)
#define LIB_TRANSFORMS_1_DIV_SQRT3 (0.5773502f)

/* Typedefs */


/* Public Function Declarations  */
// [impl->fw~mc_013~1]
static inline void lib_transforms_clarke(
    // inputs
    float32_t a,
    float32_t b,
    float32_t c,
    // outputs
    float32_t * alpha,
    float32_t * beta
)
{
    if ((alpha != NULL) && (beta != NULL))
    {
        *alpha = (2.0f * a - b - c) / 3.0f;
        *beta = (b - c) / LIB_TRANSFORMS_SQRT3;
    }
}

// [impl->fw~mc_013~1]
static inline void lib_transforms_park(
    // inputs
    float32_t alpha,
    float32_t beta,
    float32_t theta,
    // outputs
    float32_t * d,
    float32_t * q
)
{
    if ((d != NULL) && (q != NULL))
    {
        const float32_t cosTheta = cosf(theta);
        const float32_t sinTheta = sinf(theta);
        *d = (alpha * cosTheta) + (beta * sinTheta);
        *q = (-alpha * sinTheta) + (beta * cosTheta);
    }
}

// [impl->fw~mc_013~1]
static inline void lib_transforms_inversePark(
    // inputs
    float32_t d,
    float32_t q,
    float32_t theta,
    // outputs
    float32_t * alpha,
    float32_t * beta
)
{
    if ((alpha != NULL) && (beta != NULL))
    {
        const float32_t cosTheta = cosf(theta);
        const float32_t sinTheta = sinf(theta);
        *alpha = (d * cosTheta) - (q * sinTheta);
        *beta = (d * sinTheta) + (q * cosTheta);
    }
}

// [impl->fw~mc_014~1]
static inline void lib_transforms_svm(
    // inputs
    float32_t vAlpha,
    float32_t vBeta,
    float32_t vBus,
    // outputs
    float32_t * d_u,
    float32_t * d_v,
    float32_t * d_w
)
{
    if ((d_u != NULL) && (d_v != NULL) && (d_w != NULL))
    {
        if (vBus <= 0.0f) // ~0V or below
        {
            *d_u = 0.5f;
            *d_v = 0.5f;
            *d_w = 0.5f;
        }
        else // vBus > 0
        {
            const float32_t limit_sq = (vBus*vBus) / 3.0f;
            const float32_t vVectorMag_sq = (vAlpha*vAlpha) + (vBeta*vBeta);

            // if current vector magnitude > V bus
            if (vVectorMag_sq > limit_sq)
            {
                const float32_t vVectorMag = sqrtf(vVectorMag_sq);
                const float32_t k = vBus * LIB_TRANSFORMS_1_DIV_SQRT3 / vVectorMag; // limit / mag

                // clamp voltage vector
                vAlpha *= k;
                vBeta *= k;
            }

            const float32_t v_u = vAlpha;
            const float32_t v_v = (-vAlpha + LIB_TRANSFORMS_SQRT3 * vBeta) / 2.0f;
            const float32_t v_w = (-vAlpha - LIB_TRANSFORMS_SQRT3 * vBeta) / 2.0f;

            const float32_t max_v = MAX_OF3(v_u, v_v, v_w);
            const float32_t min_v = MIN_OF3(v_u, v_v, v_w);
            const float32_t offset = ((max_v + min_v) / 2.0f);

            const float32_t invVBus = 1.0f / vBus;

            *d_u = 0.5f + (v_u - offset) * invVBus;
            *d_v = 0.5f + (v_v - offset) * invVBus;
            *d_w = 0.5f + (v_w - offset) * invVBus;
        }
    }
}
#ifdef __cplusplus
}
#endif
