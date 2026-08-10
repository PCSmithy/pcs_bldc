#pragma once

/* Includes */
#include "lib_types.h"

#include "IO_AS5048.h"

#include "dev_switch.h"

#include "app_motorControl.h"
#include "app_rgbLedRing.h"

/* Defines */

// BENCH-ONLY test hook (never merge with this set nonzero):
//   0 — stock behavior.
//   1 — stall-R capture: the run toggle walks a fixed duty-cycle schedule and
//       telemetry (main.c) trims to that test's signal set.
//   2 — spin/BEMF capture: controls stay stock (leave the bridge disarmed);
//       telemetry trims to rotor angle + the three phase terminal voltages.
#ifndef PCS_BENCH_DUTY_SEQ
#define PCS_BENCH_DUTY_SEQ (0)
#endif

/* Typedefs */

typedef enum
{
    APP_USERCONTROLS_MODE_OFF,
    APP_USERCONTROLS_MODE_VELOCITY,
    // APP_USERCONTROLS_MODE_POSITION, // TODO
    APP_USERCONTROLS_MODE_COUNT,
} app_userControls_mode_E;

typedef struct
{
    app_motorControl_channel_E motor;
    dev_switch_channel_E button;
    IO_AS5048_channel_E dial; // refactor into dev_encoder at some point
    app_rgbLedRing_channel_E ring; // ring whose display mode the button cycles
} app_userControls_config_S;

/* Public Function Declarations */

bool app_userControls_init(const app_userControls_config_S * const config);
void app_userControls_run1ms(void);

#if (PCS_BENCH_DUTY_SEQ == 1)
// The bench schedule's currently commanded duty, for telemetry.
float32_t app_userControls_benchDuty01(void);
#endif