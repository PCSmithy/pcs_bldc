#pragma once

// The app_motorControl test harness the three suites share: the module over the
// REAL lib_timer (on a fake clock) and the REAL IO_bridge (over the shared HW_TIM /
// HW_ADC mocks), with boundary doubles for the rotor estimate, encoder and gate driver.

#include "lib_utils.h"
#include "app_motorControl.h"
#include "mock_app_rotorEstimation.h"
#include "mock_IO_AS5048.h"
#include "mock_dev_gateDriver.h"
#include "mock_HW_TIM.h"
#include "mock_HW_ADC.h"
#include "unity.h"
#include <math.h>

/* ---- the test config ---- */
#define PERIOD          (4000U)         // HW_TIM ARR; duty math is exact against it
#define BRIDGE_PERIPH   (HW_TIM_PERIPHERAL_1)
#define MOTOR_BRIDGE    (IO_BRIDGE_CHANNEL_MOTOR)
#define MOTOR_MC        (APP_MOTORCONTROL_CHANNEL_MAIN)
#define GD_MAIN         (DEV_GATEDRIVER_CHANNEL_MAIN)
#define ENC_MOTOR       (IO_AS5048_CHANNEL_MOTOR)

#define PHASE_TRIP_A    (1.2f)
#define BUS_TRIP_A      (1.5f)
#define ENCODER_FAULT_LIMIT (5U)
#define MAX_VELOCITY    (20.0f)         // rad/s
#define ALIGN_DUTY_01   (0.3f)
#define MAX_DUTY_01     (0.9f)          // the module's six-step ceiling
#define PWM_PERIOD_S    (50.0e-6f)
#define TEST_VBUS_V     (24.0f)

// K_p = 8 V/A and K_i T = 0.4 V/A a step; K_t, K_vp, K_vi round for the velocity tests
#define FOC_I_MAX       (1.0f)
#define FOC_L           (4.0e-3f)
#define FOC_R           (4.0f)
#define FOC_BW          (2000.0f)
#define FOC_KP          (FOC_L * FOC_BW)
#define FOC_KI_T        (FOC_R * FOC_BW * PWM_PERIOD_S)
#define FOC_KT          (0.5f)
#define FOC_KVP         (0.01f)
#define FOC_KVI         (0.05f)
#define POS_KPP         (30.0f)
#define POS_KPI         (90.0f)
#define POS_MAX         (20.0f * 3.14159265f)

#define TWO_PI_F        (6.28318530718f)
#define SQRT3_F         (1.7320508f)
#define DEG2RAD(d)      ((float32_t)(d) * 3.14159265f / 180.0f)
#define RAD2DEG(r)      ((float32_t)(r) * 180.0f / 3.14159265f)
#define DUTY_COMPARE(duty01) ((uint32_t)(((duty01) * (float32_t)PERIOD) + 0.5f))   // as IO_bridge rounds

// The 500 ms alignment dwell and the 5 ms precharge, each cleared with a margin.
#define ALIGN_ADVANCE_MS     (600U)
#define PRECHARGE_MS         (5U)
#define PRECHARGE_ADVANCE_MS (PRECHARGE_MS + 1U)

/* ---- config and clock ---- */
void advanceTime_ms(uint32_t ms);
app_motorControl_channelConfig_S * freshConfig(void);   // the test config rebuilt, to mutate before reinit()
bool reinit(void);

/* ---- inputs ---- */
void setPhaseCurrentsUV(float32_t ampsU, float32_t ampsV);   // W derives as -(U + V)
void setBusCurrent(float32_t amps);
void setBusVoltage(float32_t volts);
void setNominalCurrents(void);

/* ---- the loop ---- */
void cycle(void);   // one PWM-cycle step on the motor's bridge
void tick(void);    // a step, the 1 ms pass, a step: one millisecond of the loop
void driveToRunning(float32_t velocity);   // six-step driving after the first alignment
void alignAtZero(void);   // aligned at rotor angle 0, six-step selected, still on the alignment pass
void commutateAt(float32_t rotorDeg, float32_t velocity);   // rotor angle and speed target, then a tick
void startMethodAt(app_motorControl_mode_E mode, float32_t rotorDeg, float32_t velocity);   // disable, enable: its first step

/* ---- observation ---- */
app_motorControl_snapshot_S snapshot(void);
float32_t profileVelocity(void);   // w_f as the last tick moved theta*; exact but on the pass that lands on the point
bool bridgeEnabled(void);
bool phaseFloating(IO_bridge_phase_E phase);
uint32_t phaseCompare(IO_bridge_phase_E phase);
uint32_t drivenCompare(void);   // the six-step pattern's duty-d phase
int observedSector(void);       // 0..5 in field-angle order from 330 deg, -1 for no six-step pattern
bool allPhasesAtZeroDuty(void);

typedef struct { float32_t angleDeg; float32_t magnitude_V; } statorVoltage_S;
typedef struct { float32_t vD; float32_t vQ; } rotorVoltage_S;
statorVoltage_S statorVoltage(void);   // the bridge's vector, recovered from the compares at TEST_VBUS_V
rotorVoltage_S rotorVoltageAt(float32_t rotorDeg);
float32_t wrapDeg(float32_t deg);
float32_t qVoltageDriftAfterTheLimit(uint32_t steps);   // bus to TEST_VBUS_V, then v_q's change over `steps` cycles
