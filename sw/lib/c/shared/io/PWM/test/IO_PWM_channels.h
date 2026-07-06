#pragma once

// Test-local phase seam: the three motor phases the bridge drives.
typedef enum
{
    IO_PWM_PHASE_U,
    IO_PWM_PHASE_V,
    IO_PWM_PHASE_W,
    IO_PWM_PHASE_COUNT,
} IO_PWM_phase_E;
