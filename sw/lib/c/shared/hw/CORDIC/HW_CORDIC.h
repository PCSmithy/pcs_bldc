#pragma once

// Sine and cosine of an angle from the trigonometric coprocessor: one
// write and two reads on the target, libm in the sim. Single-instance and
// one caller context at a time: a caller that preempts an evaluation
// between its write and its reads takes the other's results. The caller
// is the commutation step in the PWM-cycle interrupt.

/* Includes */
#include "lib_types.h"

/* Target Config */
#include "HW_CORDIC_target.h"   // HW_CORDIC_config_S

#ifdef __cplusplus
extern "C" {
#endif

/* Public Function Declarations */
bool HW_CORDIC_init(const HW_CORDIC_config_S * const config);

// Sine and cosine of theta_rad, any angle of magnitude up to 4 pi.
// False, outputs untouched, before init or on a NULL output.
bool HW_CORDIC_sinCos(float32_t theta_rad, float32_t * const sin_out, float32_t * const cos_out);

#ifdef __cplusplus
}
#endif
