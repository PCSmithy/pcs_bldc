#pragma once

/* Includes */

/* Defines */

// The three motor phases of the power bridge, in U/V/W order.
typedef enum
{
    IO_PWM_PHASE_U,
    IO_PWM_PHASE_V,
    IO_PWM_PHASE_W,
    IO_PWM_PHASE_COUNT,
} IO_PWM_phase_E;
