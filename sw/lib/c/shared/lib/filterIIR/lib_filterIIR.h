#pragma once

/* Includes */
#include "lib_types.h"
#include "lib_utils.h"


/* Defines */

/* Typedefs */

// TODO: Butterworth/Chebyshev/Bessel variants
typedef enum
{
    LIB_FILTERIIR_TYPE_EMA,
    LIB_FILTERIIR_TYPE_COUNT,
} lib_filterIIR_type_E;

// Fields carry difference-equation notation: y[k], y[k-1], x[k].
typedef struct
{
    float32_t alpha;   // smoothing factor in (0, 1]; alpha = dt/tau for a
                       // first-order low-pass of time constant tau
    float32_t y_k;     // output
    float32_t y_k_1;   // output [k-1]
    float32_t x_k;     // input
} lib_filterIIR_ema_S;

typedef struct
{
    lib_filterIIR_type_E type;
    union
    {
        lib_filterIIR_ema_S ema;
    };
    bool init;
} lib_filterIIR_channel_S;


/* Public Function Declarations  */

// Validate the channel's type + coefficients and seed its state: the output
// starts at the current input (set x_k first for a transient-free start).
bool lib_filterIIR_init(lib_filterIIR_channel_S * const filter);

// Advance the filter one sample
void lib_filterIIR_update(lib_filterIIR_channel_S * const filter);
