// Unit tests for app_motorControl's overcurrent trip (fw~safety_001) and enable
// gating (fw~mc_006), exercised through a hybrid harness:
//
//   - REAL lib_timer, driven by a fake clock (a getTime_us function pointer) —
//     so the real expiry logic runs and the 500 ms alignment dwell is advanced
//     deterministically instead of waited out.
//   - REAL IO_bridge over the shared HW_TIM / HW_ADC mocks — so injecting a
//     sensed phase/bus voltage runs the real current-sense scaling and the
//     bridge enable path end-to-end. The bridge's live enable is observed via
//     the HW_TIM peripheral's master output enable (MOE).
//   - Boundary doubles for app_rotorEstimation (set the rotor angle and
//     velocity), IO_AS5048 (the fault status) and dev_gateDriver (force
//     operational) — inputs whose real chains (the tracking loop, SPI frames,
//     I2C status bytes) are irrelevant to what these tests assert.

#include "app_motorControl.h"
#include "mock_app_rotorEstimation.h"
#include "mock_IO_AS5048.h"
#include "mock_dev_gateDriver.h"
#include "mock_HW_TIM.h"
#include "mock_HW_ADC.h"
#include "lib_timer.h"
#include "HW_CORDIC.h"
#include "unity.h"
#include <math.h>

/* ---- fake clock: the only thing lib_timer bottoms out on ---- */
// Monotonic across the whole run (never rewound), so lib_timer's delta
// accumulation stays correct; tests advance it forward only.
static uint64_t g_now_us;
static uint32_t fakeNow_us(void) { return (uint32_t)g_now_us; }
const lib_timer_config_S lib_timer_config = { .getTime_us = fakeNow_us };

static void advanceTime_ms(uint32_t ms) { g_now_us += (uint64_t)ms * 1000U; }

/* ---- board-shaped constants ---- */
#define PERIOD          (4000U)         // HW_TIM ARR; duty math is exact against it
#define BRIDGE_PERIPH   (HW_TIM_PERIPHERAL_1)
#define MOTOR_BRIDGE    (IO_BRIDGE_CHANNEL_MOTOR)
#define MOTOR_MC        (APP_MOTORCONTROL_CHANNEL_MAIN)
#define GD_MAIN         (DEV_GATEDRIVER_CHANNEL_MAIN)
#define ENC_MOTOR       (IO_AS5048_CHANNEL_MOTOR)

// Trip thresholds under test (mirror app_motorControl.cpp).
#define PHASE_TRIP_A    (1.2f)
#define BUS_TRIP_A      (1.5f)

// Drive parameters (mirror the module + the config above).
#define MAX_VELOCITY    (20.0f)         // configured max speed (rad/s)
#define ALIGN_DUTY_01   (0.3f)          // mirrors ALIGNMENT_DUTY_CYCLE
#define MAX_DUTY_01     (0.9f)          // mirrors APP_MOTORCONTROL_MAX_DUTY_01
#define FOC_I_MAX       (1.0f)          // the test config's FOC block: I_max, L, R, w_bw
#define FOC_L           (4.0e-3f)
#define FOC_R           (4.0f)
#define FOC_BW          (2000.0f)
#define FOC_KP          (FOC_L * FOC_BW)                // 8 V/A
#define FOC_KI_T        (FOC_R * FOC_BW * 50.0e-6f)    // 0.4 V/A: the integral's growth per step
#define FOC_KT          (0.5f)          // K_t, K_vp, K_vi: round numbers for the velocity tests
#define FOC_KVP         (0.01f)
#define FOC_KVI         (0.05f)
#define DEG2RAD(d)      ((float32_t)(d) * 3.14159265f / 180.0f)
// Compare-register value for a duty against PERIOD (matches IO_bridge's
// (uint32_t)(duty*period + 0.5f)).
#define DUTY_COMPARE(duty01) ((uint32_t)(((duty01) * (float32_t)PERIOD) + 0.5f))

// Alignment dwell (mirrors app_motorControl.cpp's ALIGNMENT_DWELL_TIMER_MS); the
// advance margin clears any ms-rounding at the dwell boundary.
#define ALIGN_ADVANCE_MS (600U)
// Bootstrap precharge on every enable (fw~mc_021); the advance clears it.
#define PRECHARGE_MS         (5U)
#define PRECHARGE_ADVANCE_MS (PRECHARGE_MS + 1U)

// Current-sense front ends (mirror IO_bridge_channels.c): phase shunts biased
// at 1.65 V / 0.1 V/A, bus shunt ground-referenced at 0.6 V/A.
#define PHASE_BIAS_V    (1.65f)
#define PHASE_V_PER_A   (0.1f)
#define BUS_V_PER_A     (0.6f)
static const struct { HW_ADC_channels_E ch; uint8_t in; } phaseSense[IO_BRIDGE_PHASE_COUNT] = {
    [IO_BRIDGE_PHASE_U] = { HW_ADC_CHANNEL_1, 6U },
    [IO_BRIDGE_PHASE_V] = { HW_ADC_CHANNEL_2, 7U },
    [IO_BRIDGE_PHASE_W] = { HW_ADC_CHANNEL_1, 8U },
};
#define BUS_CH          (HW_ADC_CHANNEL_2)
#define BUS_IN          (11U)
#define BUS_V_CH        (HW_ADC_CHANNEL_1)   // bus-voltage divider, 0.15 V/V, as on the board
#define BUS_V_IN        (12U)
#define BUS_V_PER_V     (0.15f)
#define TEST_VBUS_V     (24.0f)
// U and V each sit at slot 0 of their own ADC's injected sequence; W is derived.
#define PHASE_U_INJ     (0U)
#define PHASE_V_INJ     (0U)
#define PAIR_WINDOW_US  (25U)
#define CREST_PERIOD_US (50U)

static IO_bridge_channelConfig_S     bridgeCfg[IO_BRIDGE_CHANNEL_COUNT];
static IO_bridge_config_S            bridgeConfig;
static app_motorControl_channelConfig_S appCfg[APP_MOTORCONTROL_CHANNEL_COUNT];
static app_motorControl_config_S     appConfig;

static void buildConfigs(void)
{
    bridgeCfg[IO_BRIDGE_CHANNEL_MOTOR] = (IO_bridge_channelConfig_S){
        .phase = {
            [IO_BRIDGE_PHASE_U] = { .tim = HW_TIM_CHANNEL_PWM_U, .complementPhase = IO_BRIDGE_PHASE_V,
                                    .currentSense = { phaseSense[0].ch, phaseSense[0].in,
                                                      PHASE_U_INJ, PHASE_BIAS_V, PHASE_V_PER_A } },
            [IO_BRIDGE_PHASE_V] = { .tim = HW_TIM_CHANNEL_PWM_V, .complementPhase = IO_BRIDGE_PHASE_U,
                                    .currentSense = { phaseSense[1].ch, phaseSense[1].in,
                                                      PHASE_V_INJ, PHASE_BIAS_V, PHASE_V_PER_A } },
            [IO_BRIDGE_PHASE_W] = { .tim = HW_TIM_CHANNEL_PWM_W, .complementPhase = IO_BRIDGE_PHASE_COUNT,
                                    .currentSense = { phaseSense[2].ch, phaseSense[2].in,
                                                      IO_BRIDGE_INJECTED_NONE, PHASE_BIAS_V, PHASE_V_PER_A } },
        },
        .busCurrent = { BUS_CH, BUS_IN, IO_BRIDGE_INJECTED_NONE, 0.0f, BUS_V_PER_A },
        .injectedPairWindow_us = PAIR_WINDOW_US,
        .zeroOffset = { .filterAlpha = 0.005f, .minSamples = 1000U },
        .busVoltage = { BUS_V_CH, BUS_V_IN, IO_BRIDGE_INJECTED_NONE, 0.0f, BUS_V_PER_V } };
    bridgeConfig = (IO_bridge_config_S){ .channels = bridgeCfg, .numChannels = IO_BRIDGE_CHANNEL_COUNT };

    appCfg[APP_MOTORCONTROL_CHANNEL_MAIN] = (app_motorControl_channelConfig_S){
        .gateDriver = GD_MAIN,
        .bridge = MOTOR_BRIDGE,
        .maxVelocity_radPerSec = MAX_VELOCITY,
        .rotorEstimator = APP_ROTORESTIMATION_CHANNEL_MAIN,
        .encoder = ENC_MOTOR,
        // One pole pair keeps electrical angle == rotor angle, so a commutation
        // test can place the field in a chosen sector by setting the rotor angle.
        .motorPolePairs = 1U,
        .pwmPeriod_s = 50.0e-6f,
        .maxVoltage_V = 10.0f,
        // K_p = 8 V/A, K_i T = 0.4 V/A per step: round numbers for the regulator tests
        .foc = { .maxCurrent_A = FOC_I_MAX, .motorL_H = FOC_L, .motorR_ohm = FOC_R, .currentLoopBandwidth_radPerSec = FOC_BW,
                 .torqueConstant_NmPerA = FOC_KT, .velocityKp_NmPerRadPerSec = FOC_KVP, .velocityKi_NmPerRad = FOC_KVI,
                 .velocityKff = 0.0f,
                 .positionKp_perSec = 30.0f, .positionKi_perSec2 = 90.0f, .maxPosition_rad = 20.0f * PI },
        .defaultMode = APP_MOTORCONTROL_MODE_SIX_STEP_TRAP,
        .prechargeTime_ms = PRECHARGE_MS };
    appConfig = (app_motorControl_config_S){ .channels = appCfg, .numChannels = APP_MOTORCONTROL_CHANNEL_COUNT };
}

/* ---- helpers ---- */

static bool bridgeEnabled(void) { return mock_HW_TIM_getMoe(BRIDGE_PERIPH); }

static app_motorControl_state_E motorState(void)
{
    app_motorControl_snapshot_S s = { 0 };
    (void)app_motorControl_getSnapshot(MOTOR_MC, &s);
    return s.state;
}

static float32_t motorSetpoint(void)
{
    app_motorControl_snapshot_S s = { 0 };
    (void)app_motorControl_getSnapshot(MOTOR_MC, &s);
    return s.velocitySetpoint_radPerSec;
}

// Phase currents reach the module only through the bridge's injected (crest)
// path: U and V are sampled and W derives as -(U + V). The bridge stamps them
// off the fake clock, which steps one PWM period per call so each pair is its
// own crest (the bridge keeps its sample times across tests).
static void setPhaseCurrentsUV(float32_t ampsU, float32_t ampsV)
{
    g_now_us += CREST_PERIOD_US;

    mock_HW_ADC_setInjectedVolts(phaseSense[IO_BRIDGE_PHASE_U].ch, PHASE_U_INJ,
                                 PHASE_BIAS_V + (ampsU * PHASE_V_PER_A));
    mock_HW_ADC_setInjectedVolts(phaseSense[IO_BRIDGE_PHASE_V].ch, PHASE_V_INJ,
                                 PHASE_BIAS_V + (ampsV * PHASE_V_PER_A));
    mock_HW_ADC_fireInjected(phaseSense[IO_BRIDGE_PHASE_U].ch, HW_ADC_CONVERSION_STATUS_OK);
    mock_HW_ADC_fireInjected(phaseSense[IO_BRIDGE_PHASE_V].ch, HW_ADC_CONVERSION_STATUS_OK);
}

static void setBusCurrent(float32_t amps)
{
    mock_HW_ADC_setVolts(BUS_CH, BUS_IN, amps * BUS_V_PER_A);
}

// The bus-voltage divider the bridge reads for its voltage-vector command.
static void setBusVoltage(float32_t volts)
{
    mock_HW_ADC_setVolts(BUS_V_CH, BUS_V_IN, volts * BUS_V_PER_V);
}

static void setNominalCurrents(void)
{
    setPhaseCurrentsUV(0.0f, 0.0f);
    setBusCurrent(0.0f);
}

// One millisecond of the drive loop. On the board the bridge's cycle callback runs
// twenty times between 1 ms passes: the cycles before a pass fetch the currents it
// latches on, the cycles after it apply what it decided. One of each stands for them.
static void tick(void)
{
    app_motorControl_run50us();
    app_motorControl_run1ms();
    app_motorControl_run50us();
}

// Bring the channel to steady commutation: operational, angle fixed, SIX_STEP
// requested, alignment dwell advanced, then a cycle to begin driving.
static void driveToRunning(float32_t velocity)
{
    mock_dev_gateDriver_setOperational(GD_MAIN, true);
    mock_app_rotorEstimation_setAngle(0.0f);
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_SIX_STEP_TRAP);
    app_motorControl_setVelocity(MOTOR_MC, velocity, 0.0f);

    tick();          // enters alignment (bridge enabled)
    advanceTime_ms(PRECHARGE_ADVANCE_MS);
    tick();          // precharge done, alignment begins
    advanceTime_ms(ALIGN_ADVANCE_MS);
    tick();          // dwell expires, captures offset
    tick();          // commutating
}

// Complete alignment at rotor angle 0 (capturing offset 0), leaving the channel
// aligned and operational but not yet commutated at a test angle.
static void alignAtZero(void)
{
    mock_dev_gateDriver_setOperational(GD_MAIN, true);
    mock_app_rotorEstimation_setAngle(0.0f);
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_SIX_STEP_TRAP);
    app_motorControl_setVelocity(MOTOR_MC, 10.0f, 0.0f);
    tick();          // alignment begins
    advanceTime_ms(PRECHARGE_ADVANCE_MS);
    tick();          // precharge done, alignment begins
    advanceTime_ms(ALIGN_ADVANCE_MS);
    tick();          // dwell expires, offset := 0
}

// Set rotor angle (degrees) and signed velocity, then run one commutation cycle.
static void commutateAt(float32_t rotorDeg, float32_t velocity)
{
    mock_app_rotorEstimation_setAngle(DEG2RAD(rotorDeg));
    app_motorControl_setVelocity(MOTOR_MC, velocity, 0.0f);
    tick();
}

/* ---- pattern observation (bridge state read back through HW_TIM) ---- */

static HW_TIM_channels_E timChannel(IO_bridge_phase_E phase)
{
    HW_TIM_channels_E ch = HW_TIM_CHANNEL_PWM_U;
    if (phase == IO_BRIDGE_PHASE_V) { ch = HW_TIM_CHANNEL_PWM_V; }
    else if (phase == IO_BRIDGE_PHASE_W) { ch = HW_TIM_CHANNEL_PWM_W; }
    return ch;
}

// A phase floats when its output is disabled (both gate lines off).
static bool phaseFloating(IO_bridge_phase_E phase) { return !mock_HW_TIM_getOutputEnabled(timChannel(phase)); }
static uint32_t phaseCompare(IO_bridge_phase_E phase) { return mock_HW_TIM_getCompare(timChannel(phase)); }

// The duty of the single driven phase carrying the duty-d command (the two
// conducting phases are one at duty01 and one at 0; the third floats).
static uint32_t drivenCompare(void)
{
    uint32_t maxCompare = 0U;
    for (size_t p = 0U; p < IO_BRIDGE_PHASE_COUNT; p++)
    {
        if (!phaseFloating((IO_bridge_phase_E)p))
        {
            const uint32_t c = phaseCompare((IO_bridge_phase_E)p);
            if (c > maxCompare) { maxCompare = c; }
        }
    }
    return maxCompare;
}

// Identify the current sector 0..5 by its (duty-d phase, floating phase) pair
// (the six-step table), or -1 if the pattern is not a single-driven sector.
static int observedSector(void)
{
    IO_bridge_phase_E dutyD = IO_BRIDGE_PHASE_COUNT;
    IO_bridge_phase_E floatP = IO_BRIDGE_PHASE_COUNT;
    for (size_t p = 0U; p < IO_BRIDGE_PHASE_COUNT; p++)
    {
        const IO_bridge_phase_E phase = (IO_bridge_phase_E)p;
        if (phaseFloating(phase)) { floatP = phase; }
        else if (phaseCompare(phase) > 0U) { dutyD = phase; }
    }

    // (dutyD, floating) in field-angle order from 330 deg: U+V-, U+W-, V+U-,
    // V+W-, W+V-, W+U- (fw~mc_011); +3 is the opposite field.
    static const struct { IO_bridge_phase_E dutyD; IO_bridge_phase_E floatP; } table[6] = {
        { IO_BRIDGE_PHASE_U, IO_BRIDGE_PHASE_W },   // 0: U d, V 0, W float
        { IO_BRIDGE_PHASE_U, IO_BRIDGE_PHASE_V },   // 1: U d, W 0, V float
        { IO_BRIDGE_PHASE_V, IO_BRIDGE_PHASE_U },   // 2: V d, W 0, U float
        { IO_BRIDGE_PHASE_V, IO_BRIDGE_PHASE_W },   // 3: V d, U 0, W float
        { IO_BRIDGE_PHASE_W, IO_BRIDGE_PHASE_V },   // 4: W d, U 0, V float
        { IO_BRIDGE_PHASE_W, IO_BRIDGE_PHASE_U },   // 5: W d, V 0, U float
    };
    int sector = -1;
    for (int i = 0; i < 6; i++)
    {
        if ((table[i].dutyD == dutyD) && (table[i].floatP == floatP)) { sector = i; }
    }
    return sector;
}

static const HW_CORDIC_config_S cordicConfig = { 0 };

void setUp(void)
{
    (void)HW_CORDIC_init(&cordicConfig);
    mock_HW_TIM_reset(PERIOD);
    mock_HW_ADC_reset();
    mock_IO_AS5048_reset();
    mock_app_rotorEstimation_reset();
    mock_dev_gateDriver_reset();
    buildConfigs();
    (void)IO_bridge_init(&bridgeConfig);
    (void)app_motorControl_init(&appConfig);
    setNominalCurrents();
    setBusVoltage(TEST_VBUS_V);
}

void tearDown(void) {}

/* ---- harness sanity: the drive path actually enables the bridge ---- */

// [test->fw~mc_006~1]
static void test_drives_bridge_when_operational_and_unfaulted(void)
{
    TEST_ASSERT_FALSE(bridgeEnabled());        // starts dark
    driveToRunning(10.0f);
    TEST_ASSERT_TRUE(bridgeEnabled());         // commutating -> bridge live
}

/* ---- fw~safety_001: overcurrent trip ---- */

// [test->fw~safety_001~1]
static void test_phase_overcurrent_trips_and_latches(void)
{
    driveToRunning(10.0f);
    TEST_ASSERT_TRUE(bridgeEnabled());

    setPhaseCurrentsUV(3.0f, 0.0f);            // U > 1.2 A
    tick();
    TEST_ASSERT_FALSE(bridgeEnabled());        // disabled within one cycle

    setNominalCurrents();                      // fault cleared physically...
    tick();
    TEST_ASSERT_FALSE(bridgeEnabled());        // ...but the latch persists
}

// [test->fw~safety_001~1]
static void test_bus_overcurrent_trips(void)
{
    driveToRunning(10.0f);
    TEST_ASSERT_TRUE(bridgeEnabled());

    setBusCurrent(2.0f);                       // > 1.5 A
    tick();
    TEST_ASSERT_FALSE(bridgeEnabled());
}

// [test->fw~safety_001~1]
static void test_current_below_threshold_does_not_trip(void)
{
    driveToRunning(10.0f);

    setPhaseCurrentsUV(1.1f, 0.0f);            // U and the derived W just under 1.2 A
    setBusCurrent(1.4f);                       // just under 1.5 A
    tick();
    TEST_ASSERT_TRUE(bridgeEnabled());         // no trip
}

// [test->fw~safety_001~1]
// A negative phase-current magnitude trips too (bipolar shunt). W is derived, so
// the scenario is expressed through the sampled legs: U + V = 1.5 A puts W at
// -1.5 A while neither sampled leg is over the trip.
static void test_negative_phase_overcurrent_trips(void)
{
    driveToRunning(10.0f);

    setPhaseCurrentsUV(0.75f, 0.75f);          // W = -(0.75 + 0.75), |-1.5| > 1.2 A
    tick();
    TEST_ASSERT_FALSE(bridgeEnabled());
}

/* ---- fw~mc_006: enable gating ---- */

// [test->fw~mc_006~1]
static void test_gate_blocks_bridge_when_not_operational(void)
{
    // Gate driver never made operational; request a full drive.
    mock_app_rotorEstimation_setAngle(0.0f);
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_SIX_STEP_TRAP);
    app_motorControl_setVelocity(MOTOR_MC, 10.0f, 0.0f);

    for (int i = 0; i < 5; i++)
    {
        tick();
        advanceTime_ms(200U);
        TEST_ASSERT_FALSE(bridgeEnabled());    // held disabled every cycle
    }

    // Becoming operational lets the drive proceed.
    mock_dev_gateDriver_setOperational(GD_MAIN, true);
    tick();                 // alignment begins -> enabled
    TEST_ASSERT_TRUE(bridgeEnabled());
}

// [test->fw~mc_006~1]
// A latched fault holds the bridge disabled even while the gate driver is
// operational — the enable request does not take effect.
static void test_gate_blocks_bridge_while_fault_latched(void)
{
    driveToRunning(10.0f);
    setPhaseCurrentsUV(3.0f, 0.0f);
    tick();
    TEST_ASSERT_FALSE(bridgeEnabled());

    setNominalCurrents();                      // currents nominal, still operational
    tick();
    tick();
    TEST_ASSERT_FALSE(bridgeEnabled());        // stays disabled: latch gates enable
}

// [test->fw~mc_006~1]
// The hygiene fix: an OFF request takes effect the same cycle (no extra drive
// cycle), even if the velocity is left non-zero.
static void test_off_request_disables_bridge_same_cycle(void)
{
    driveToRunning(10.0f);
    TEST_ASSERT_TRUE(bridgeEnabled());

    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_OFF);
    tick();
    TEST_ASSERT_FALSE(bridgeEnabled());
}

// [test->fw~mc_006~1]
// A zero speed target while enabled and aligned idles the phases at zero duty
// but holds the master output enable asserted — the bridge stays up across a
// run of zero-demand cycles (no flapping), and a fault still forces it off.
static void test_zero_demand_holds_bridge_enabled(void)
{
    alignAtZero();                             // aligned, operational, dwell done

    commutateAt(30.0f, 0.0f);                  // zero demand while enabled + aligned
    TEST_ASSERT_TRUE(bridgeEnabled());         // MOE stays asserted at zero duty
    TEST_ASSERT_EQUAL_UINT32(0U, drivenCompare());   // phases idle at zero duty

    for (int i = 0; i < 8; i++)
    {
        commutateAt(30.0f, 0.0f);
        TEST_ASSERT_TRUE(bridgeEnabled());     // does not flap cycle to cycle
    }

    setPhaseCurrentsUV(3.0f, 0.0f);            // a fault still kills the bridge
    tick();
    TEST_ASSERT_FALSE(bridgeEnabled());
}

/* ---- state view + fault-clear (data contract for the ring, fw~mc_009) ---- */

// The snapshot state tracks disabled -> enabled -> faulted.
static void test_state_view_tracks_disabled_enabled_faulted(void)
{
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_STATE_DISABLED, motorState());   // idle at init

    driveToRunning(10.0f);
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_STATE_ENABLED, motorState());    // commutating

    setPhaseCurrentsUV(3.0f, 0.0f);
    tick();
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_STATE_FAULTED, motorState());    // tripped
}

// The commanded speed target is exposed, signed.
static void test_velocity_setpoint_exposed(void)
{
    driveToRunning(10.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.0f, motorSetpoint());

    commutateAt(30.0f, -5.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -5.0f, motorSetpoint());
}

/* ---- the velocity view: the rotor estimator's ---- */

static float32_t motorVelocity(void)
{
    app_motorControl_snapshot_S s = { 0 };
    (void)app_motorControl_getSnapshot(MOTOR_MC, &s);
    return s.velocityMeasured_radPerSec;
}

// The snapshot's measured velocity is the estimator's (fw~est_rotor_002), as
// read on the 1 ms pass, sign included.
static void test_velocity_view_is_the_estimators(void)
{
    mock_app_rotorEstimation_setVelocity(12.5f);
    tick();
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 12.5f, motorVelocity());

    mock_app_rotorEstimation_setVelocity(-3.25f);
    tick();
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, -3.25f, motorVelocity());
}

// [test->fw~safety_001~1]
// clearFault releases the latch; with currents nominal the drive resumes.
static void test_clear_fault_releases_latch_and_resumes(void)
{
    driveToRunning(10.0f);
    setPhaseCurrentsUV(3.0f, 0.0f);
    tick();
    TEST_ASSERT_FALSE(bridgeEnabled());
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_STATE_FAULTED, motorState());

    setNominalCurrents();
    app_motorControl_clearFault(MOTOR_MC);
    tick();
    TEST_ASSERT_TRUE(bridgeEnabled());                                  // drive resumes
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_STATE_ENABLED, motorState());
}

static uint16_t faultLatches(void)
{
    app_motorControl_snapshot_S s = { 0 };
    (void)app_motorControl_getSnapshot(MOTOR_MC, &s);
    return s.faultLatches;
}

// [test->fw~ops_002~1]
// Each latch counts once: not again while it holds, again after a clear and
// a new trip.
static void test_fault_latches_are_counted(void)
{
    driveToRunning(10.0f);
    TEST_ASSERT_EQUAL_UINT16(0U, faultLatches());
    setPhaseCurrentsUV(3.0f, 0.0f);
    tick();
    TEST_ASSERT_EQUAL_UINT16(1U, faultLatches());
    tick();
    TEST_ASSERT_EQUAL_UINT16(1U, faultLatches());

    setNominalCurrents();
    app_motorControl_clearFault(MOTOR_MC);
    tick();
    TEST_ASSERT_EQUAL_UINT16(1U, faultLatches());
    setPhaseCurrentsUV(3.0f, 0.0f);
    tick();
    TEST_ASSERT_EQUAL_UINT16(2U, faultLatches());
}

/* ---- fw~safety_002: encoder-fault trip ---- */

// [test->fw~safety_002~1]
// More than 5 consecutive invalid encoder reads latch a fault; 5 do not.
static void test_encoder_fault_latches_past_limit(void)
{
    driveToRunning(10.0f);
    TEST_ASSERT_TRUE(bridgeEnabled());

    mock_IO_AS5048_setStatus(ENC_MOTOR, IO_AS5048_STATUS_FAULT);
    for (int i = 0; i < 5; i++) { tick(); }
    TEST_ASSERT_TRUE(bridgeEnabled());                 // 5 is not > 5

    tick();                         // the 6th
    TEST_ASSERT_FALSE(bridgeEnabled());
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_STATE_FAULTED, motorState());
}

// [test->fw~safety_002~1]
// A valid read resets the consecutive count, so interrupted glitches never trip.
static void test_encoder_fault_count_resets_on_valid_read(void)
{
    driveToRunning(10.0f);

    mock_IO_AS5048_setStatus(ENC_MOTOR, IO_AS5048_STATUS_FAULT);
    for (int i = 0; i < 5; i++) { tick(); }

    mock_IO_AS5048_setStatus(ENC_MOTOR, IO_AS5048_STATUS_OK);
    tick();                         // resets the count

    mock_IO_AS5048_setStatus(ENC_MOTOR, IO_AS5048_STATUS_FAULT);
    for (int i = 0; i < 5; i++) { tick(); }
    TEST_ASSERT_TRUE(bridgeEnabled());                 // never 6 in a row
}

/* ---- fw~mc_011: six-step commutation ---- */

// [test->fw~mc_011~1]
// Each of the six field sectors drives its duty-d phase at the target duty and
// its duty-0 phase at zero, with the third phase floating (output disabled).
static void test_sector_table_drives_correct_phases(void)
{
    // rotor angle -> sector (pole pairs = 1, offset 0, +lead): the field angle
    // is rotorDeg + 90 (lead) + 30 (nearest-sector bias).
    static const struct {
        float32_t rotorDeg;
        IO_bridge_phase_E dutyD;
        IO_bridge_phase_E duty0;
        IO_bridge_phase_E floatP;
    } cases[6] = {
        // rotor + 90 deg lead lands mid-sector: 30 / 90 / 150 / 210 / 270 / 330
        { 300.0f, IO_BRIDGE_PHASE_U, IO_BRIDGE_PHASE_W, IO_BRIDGE_PHASE_V },
        {   0.0f, IO_BRIDGE_PHASE_V, IO_BRIDGE_PHASE_W, IO_BRIDGE_PHASE_U },
        {  60.0f, IO_BRIDGE_PHASE_V, IO_BRIDGE_PHASE_U, IO_BRIDGE_PHASE_W },
        { 120.0f, IO_BRIDGE_PHASE_W, IO_BRIDGE_PHASE_U, IO_BRIDGE_PHASE_V },
        { 180.0f, IO_BRIDGE_PHASE_W, IO_BRIDGE_PHASE_V, IO_BRIDGE_PHASE_U },
        { 240.0f, IO_BRIDGE_PHASE_U, IO_BRIDGE_PHASE_V, IO_BRIDGE_PHASE_W },
    };

    alignAtZero();
    for (int i = 0; i < 6; i++)
    {
        commutateAt(cases[i].rotorDeg, 10.0f);   // +v, duty 0.5
        TEST_ASSERT_TRUE_MESSAGE(phaseFloating(cases[i].floatP), "floating phase should be disabled");
        TEST_ASSERT_FALSE_MESSAGE(phaseFloating(cases[i].dutyD), "duty-d phase should be driven");
        TEST_ASSERT_FALSE_MESSAGE(phaseFloating(cases[i].duty0), "duty-0 phase should be driven");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(DUTY_COMPARE(0.5f), phaseCompare(cases[i].dutyD), "duty-d compare");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, phaseCompare(cases[i].duty0), "duty-0 phase not at zero duty");
    }
}

// [test->fw~mc_011~1]
// Reversing the target sign flips the applied field by 180deg -> the sector
// shifts by 3 (of 6): the drive direction reverses.
static void test_reversal_flips_drive_direction(void)
{
    static const float32_t angles[3] = { 45.0f, 135.0f, 250.0f };
    alignAtZero();
    for (int i = 0; i < 3; i++)
    {
        commutateAt(angles[i], 10.0f);
        const int fwd = observedSector();
        commutateAt(angles[i], -10.0f);
        const int rev = observedSector();
        TEST_ASSERT_NOT_EQUAL(-1, fwd);
        TEST_ASSERT_NOT_EQUAL(-1, rev);
        TEST_ASSERT_EQUAL_INT(((fwd + 3) % 6), rev);
    }
}

// [test->fw~mc_011~1]
// Duty tracks the target magnitude and clamps to the configured maximum.
static void test_duty_proportional_and_clamped(void)
{
    alignAtZero();

    commutateAt(30.0f, 10.0f);    // 10 / 20 = 0.50
    TEST_ASSERT_EQUAL_UINT32(DUTY_COMPARE(0.5f), drivenCompare());

    commutateAt(30.0f, 5.0f);     // 5 / 20 = 0.25
    TEST_ASSERT_EQUAL_UINT32(DUTY_COMPARE(0.25f), drivenCompare());

    commutateAt(30.0f, 100.0f);   // >> max -> clamped to MAX_DUTY_01
    TEST_ASSERT_EQUAL_UINT32(DUTY_COMPARE(MAX_DUTY_01), drivenCompare());
}

/* ---- fw~mc_020: sensored sinusoidal commutation ---- */

// The stator voltage vector the bridge was handed, recovered from the three
// compares: duties off the period, the zero-sequence offset cancelling in the
// differences (fw~mc_014), scaled by the bus voltage the bridge read.
static void vectorFromCompares(float32_t * const angleDeg, float32_t * const magnitude_V)
{
    const float32_t dU = (float32_t)phaseCompare(IO_BRIDGE_PHASE_U) / (float32_t)PERIOD;
    const float32_t dV = (float32_t)phaseCompare(IO_BRIDGE_PHASE_V) / (float32_t)PERIOD;
    const float32_t dW = (float32_t)phaseCompare(IO_BRIDGE_PHASE_W) / (float32_t)PERIOD;
    const float32_t vAlpha = TEST_VBUS_V * ((2.0f * dU) - dV - dW) / 3.0f;
    const float32_t vBeta  = TEST_VBUS_V * (dV - dW) / 1.7320508f;
    *angleDeg    = atan2f(vBeta, vAlpha) * 180.0f / 3.14159265f;
    *magnitude_V = sqrtf((vAlpha * vAlpha) + (vBeta * vBeta));
}

static float32_t wrapDeg(float32_t deg)
{
    while (deg > 180.0f)  { deg -= 360.0f; }
    while (deg <= -180.0f) { deg += 360.0f; }
    return deg;
}

// [test->fw~mc_020~1]
// (0, V) at the rotor angle lands the stator vector 90 deg ahead of the rotor
// for a positive target and 90 deg behind for a negative one, at
// |V| = (|target| / max velocity) x V_max, clamped at V_max.
static void test_sinusoidal_vector_sits_on_the_q_axis(void)
{
    alignAtZero();
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_SINUSOIDAL);

    static const struct { float32_t rotorDeg; float32_t velocity; float32_t leadDeg; float32_t volts; } cases[] = {
        {  30.0f,  10.0f,  90.0f,  5.0f },   // half scale
        {  30.0f, -10.0f, -90.0f,  5.0f },   // reversed
        { 200.0f,  20.0f,  90.0f, 10.0f },   // full scale
        { 200.0f,  30.0f,  90.0f, 10.0f },   // past full scale: setVelocity saturates, V holds V_max
        { 300.0f,  -4.0f, -90.0f,  2.0f },
    };
    for (size_t i = 0U; i < COUNTOF(cases); i++)
    {
        commutateAt(cases[i].rotorDeg, cases[i].velocity);
        float32_t angleDeg = 0.0f;
        float32_t volts = 0.0f;
        vectorFromCompares(&angleDeg, &volts);
        TEST_ASSERT_FLOAT_WITHIN(0.5f, 0.0f, wrapDeg(angleDeg - (cases[i].rotorDeg + cases[i].leadDeg)));
        TEST_ASSERT_FLOAT_WITHIN(0.05f, cases[i].volts, volts);
        TEST_ASSERT_TRUE(bridgeEnabled());
        TEST_ASSERT_FALSE(phaseFloating(IO_BRIDGE_PHASE_U));
        TEST_ASSERT_FALSE(phaseFloating(IO_BRIDGE_PHASE_V));
        TEST_ASSERT_FALSE(phaseFloating(IO_BRIDGE_PHASE_W));
    }
}

// [test->fw~mc_020~1]
static void test_sinusoidal_zero_target_centres_the_duties_with_the_bridge_up(void)
{
    alignAtZero();
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_SINUSOIDAL);
    commutateAt(45.0f, 0.0f);
    TEST_ASSERT_EQUAL_UINT32(PERIOD / 2U, phaseCompare(IO_BRIDGE_PHASE_U));
    TEST_ASSERT_EQUAL_UINT32(PERIOD / 2U, phaseCompare(IO_BRIDGE_PHASE_V));
    TEST_ASSERT_EQUAL_UINT32(PERIOD / 2U, phaseCompare(IO_BRIDGE_PHASE_W));
    TEST_ASSERT_TRUE(bridgeEnabled());
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_STATE_ENABLED, motorState());
}

/* ---- fw~mc_022..024: the field-oriented method ---- */

// The bridge's vector in the rotor frame: the recovered stator vector rotated
// back by the rotor angle.
static void rotorFrameFromCompares(float32_t rotorDeg, float32_t * const vD, float32_t * const vQ)
{
    float32_t angleDeg = 0.0f;
    float32_t volts = 0.0f;
    vectorFromCompares(&angleDeg, &volts);
    const float32_t rel = DEG2RAD(angleDeg - rotorDeg);
    *vD = volts * cosf(rel);
    *vQ = volts * sinf(rel);
}

// Re-select FOC (through a SINUSOIDAL detour, so the selection is new) and run
// one cycle at the rotor angle and velocity: the observed step is the method's
// first, integrals at zero.
static void startFocAt(float32_t rotorDeg, float32_t velocity)
{
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_SINUSOIDAL);
    tick();
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_FOC_TORQUE);
    commutateAt(rotorDeg, velocity);
}

// The velocity method's first step, as startFocAt but selecting FOC_VELOCITY,
// at a speed target.
static void startFocVelocityAt(float32_t rotorDeg, float32_t velocityTarget)
{
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_SINUSOIDAL);
    tick();
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_FOC_VELOCITY);
    commutateAt(rotorDeg, velocityTarget);
}

// Re-init with the cogging model's first harmonic (order 28) and second (order
// 56) at the given amplitudes and phases and the compensation gain, then align.
static void alignWithCogging(float32_t a1, float32_t phi1, float32_t a2, float32_t phi2, float32_t kcog)
{
    buildConfigs();
    appCfg[APP_MOTORCONTROL_CHANNEL_MAIN].foc.cogging[0] = (app_motorControl_cogHarmonic_S){ .order_perRev = 28U, .amplitude_Nm = a1, .phase_rad = phi1 };
    appCfg[APP_MOTORCONTROL_CHANNEL_MAIN].foc.cogging[1] = (app_motorControl_cogHarmonic_S){ .order_perRev = 56U, .amplitude_Nm = a2, .phase_rad = phi2 };
    appCfg[APP_MOTORCONTROL_CHANNEL_MAIN].foc.coggingKff = kcog;
    TEST_ASSERT_TRUE(app_motorControl_init(&appConfig));
    alignAtZero();
}

// The rotor angle in degrees at which sin(28 theta) = 1, and = -1.
#define COG_PEAK_DEG    (90.0f / 28.0f)
#define COG_TROUGH_DEG  (270.0f / 28.0f)

// [test->fw~mc_028~1]
// Both methods at zero demand: the first step commands -k_cog A sin(28 theta) / K_t
// on the q axis, so -0.1 Nm's worth at the peak and nothing at theta = 0.
static void test_cogging_compensation_subtracts_the_model_from_the_demand(void)
{
    const float32_t comp = 0.5f * 0.2f;   // k_cog A
    const float32_t tolerance = 0.01f * FOC_KP * comp / FOC_KT;
    float32_t vD = 0.0f;
    float32_t vQ = 0.0f;
    alignWithCogging(0.2f, 0.0f, 0.0f, 0.0f, 0.5f);

    startFocAt(COG_PEAK_DEG, 0.0f);
    rotorFrameFromCompares(COG_PEAK_DEG, &vD, &vQ);
    TEST_ASSERT_FLOAT_WITHIN(tolerance, -FOC_KP * comp / FOC_KT, vQ);
    TEST_ASSERT_FLOAT_WITHIN(tolerance, 0.0f, vD);
    startFocAt(0.0f, 0.0f);
    rotorFrameFromCompares(0.0f, &vD, &vQ);
    TEST_ASSERT_FLOAT_WITHIN(tolerance, 0.0f, vQ);

    startFocVelocityAt(COG_PEAK_DEG, 0.0f);
    rotorFrameFromCompares(COG_PEAK_DEG, &vD, &vQ);
    TEST_ASSERT_FLOAT_WITHIN(tolerance, -FOC_KP * comp / FOC_KT, vQ);
    TEST_ASSERT_FLOAT_WITHIN(tolerance, 0.0f, vD);
    startFocVelocityAt(0.0f, 0.0f);
    rotorFrameFromCompares(0.0f, &vD, &vQ);
    TEST_ASSERT_FLOAT_WITHIN(tolerance, 0.0f, vQ);
}

// [test->fw~mc_028~1]
// The second harmonic alone at phase pi/2: at theta = 0 it is at its peak.
static void test_cogging_second_harmonic_carries_its_phase(void)
{
    float32_t vD = 0.0f;
    float32_t vQ = 0.0f;
    alignWithCogging(0.0f, 0.0f, 0.2f, 3.14159265f / 2.0f, 1.0f);
    startFocAt(0.0f, 0.0f);
    rotorFrameFromCompares(0.0f, &vD, &vQ);
    TEST_ASSERT_FLOAT_WITHIN(0.01f * FOC_KP * 0.2f / FOC_KT, -FOC_KP * 0.2f / FOC_KT, vQ);
}

// [test->fw~mc_028~1]
// A full-scale torque target with the model at its trough adds to the demand:
// the sum is clamped to K_t I_max and the first step commands I_max.
static void test_cogging_compensation_is_clamped_with_the_demand(void)
{
    float32_t vD = 0.0f;
    float32_t vQ = 0.0f;
    alignWithCogging(0.2f, 0.0f, 0.0f, 0.0f, 1.0f);
    startFocAt(COG_TROUGH_DEG, MAX_VELOCITY);   // full-scale speed is K_t I_max under the torque method
    rotorFrameFromCompares(COG_TROUGH_DEG, &vD, &vQ);
    TEST_ASSERT_FLOAT_WITHIN(0.01f * FOC_KP * FOC_I_MAX, FOC_KP * FOC_I_MAX, vQ);
}

// [test->fw~mc_016~1]
// The estimator's torque input carries the compensated cogging share under a
// field-oriented method and K_t I_q alone under six-step.
static void test_estimator_torque_input_carries_the_known_cogging_share(void)
{
    alignWithCogging(0.2f, 0.0f, 0.0f, 0.0f, 0.5f);   // six-step selected by the alignment
    commutateAt(COG_PEAK_DEG, 10.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, mock_app_rotorEstimation_torqueInput());   // currents at zero: K_t I_q = 0
    startFocAt(COG_PEAK_DEG, 0.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.1f, mock_app_rotorEstimation_torqueInput());
}

// Re-init with the FOC feed-forward gain `kff`, then align.
static void alignWithFeedForward(float32_t kff)
{
    buildConfigs();
    appCfg[APP_MOTORCONTROL_CHANNEL_MAIN].foc.velocityKff = kff;
    TEST_ASSERT_TRUE(app_motorControl_init(&appConfig));
    alignAtZero();
}

// [test->fw~mc_025~1]
// The estimate's velocity and the target both zero, T_d at D: the method's
// first step commands i_q* = -k_ff D / K_t, read as K_p i_q* on the q axis.
static void test_foc_velocity_feeds_the_disturbance_estimate_forward(void)
{
    const float32_t D = 0.2f;   // Nm, under K_t I_max = 0.5
    alignWithFeedForward(0.5f);
    mock_app_rotorEstimation_setUnmodeledTorque(D);
    startFocVelocityAt(0.0f, 0.0f);
    float32_t vD = 0.0f;
    float32_t vQ = 0.0f;
    rotorFrameFromCompares(0.0f, &vD, &vQ);
    const float32_t iQ = -0.5f * D / FOC_KT;
    TEST_ASSERT_FLOAT_WITHIN(0.01f * FOC_KP * fabsf(iQ), FOC_KP * iQ, vQ);
    TEST_ASSERT_FLOAT_WITHIN(0.01f * FOC_KP * fabsf(iQ), 0.0f, vD);
}

// [test->fw~mc_025~1]
// k_ff = 1 and T_d = -2 K_t I_max: the first step clamps to +I_max and the
// integral is solved net of the feed-forward, so with T_d gone the next steps
// are -I_max. A tick runs two steps: the clamp step's current-loop integral
// is cancelled by the first -I_max step, so the readback is K_p (-I_max).
static void test_foc_velocity_back_calculates_the_integral_net_of_the_feed_forward(void)
{
    alignWithFeedForward(1.0f);
    mock_app_rotorEstimation_setUnmodeledTorque(-2.0f * FOC_KT * FOC_I_MAX);
    startFocVelocityAt(0.0f, 0.0f);
    float32_t vD = 0.0f;
    float32_t vQ = 0.0f;
    rotorFrameFromCompares(0.0f, &vD, &vQ);
    TEST_ASSERT_FLOAT_WITHIN(0.01f * FOC_KP * FOC_I_MAX, FOC_KP * FOC_I_MAX, vQ);

    mock_app_rotorEstimation_setUnmodeledTorque(0.0f);
    commutateAt(0.0f, 0.0f);
    rotorFrameFromCompares(0.0f, &vD, &vQ);
    TEST_ASSERT_FLOAT_WITHIN(0.01f * FOC_KP * FOC_I_MAX, -FOC_KP * FOC_I_MAX, vQ);
}

// [test->fw~mc_022~1]
// Currents at zero: the first step is (0, K_p i_q*) in the rotor frame, i_q* the
// signed speed fraction of I_max, every phase enabled.
static void test_foc_first_step_is_kp_times_the_current_target(void)
{
    alignAtZero();
    static const struct { float32_t rotorDeg; float32_t velocity; float32_t iQ; } cases[] = {
        {  30.0f,  10.0f,  0.5f },   // half scale
        {  30.0f, -10.0f, -0.5f },   // reversed
        { 200.0f,  20.0f,  1.0f },   // full scale
        { 200.0f,  30.0f,  1.0f },   // past full scale: setVelocity saturates
        { 120.0f, -20.0f, -1.0f },   // full scale reversed
        { 300.0f,  -4.0f, -0.2f },
        {  45.0f,   0.0f,  0.0f },
    };
    for (size_t i = 0U; i < COUNTOF(cases); i++)
    {
        startFocAt(cases[i].rotorDeg, cases[i].velocity);
        float32_t vD = 0.0f;
        float32_t vQ = 0.0f;
        rotorFrameFromCompares(cases[i].rotorDeg, &vD, &vQ);
        TEST_ASSERT_FLOAT_WITHIN(0.01f * FOC_KP * FOC_I_MAX, FOC_KP * cases[i].iQ, vQ);
        TEST_ASSERT_FLOAT_WITHIN(0.01f * FOC_KP * FOC_I_MAX, 0.0f, vD);
        TEST_ASSERT_TRUE(bridgeEnabled());
        TEST_ASSERT_FALSE(phaseFloating(IO_BRIDGE_PHASE_U));
        TEST_ASSERT_FALSE(phaseFloating(IO_BRIDGE_PHASE_V));
        TEST_ASSERT_FALSE(phaseFloating(IO_BRIDGE_PHASE_W));
    }
}

// [test->fw~mc_023~1]
// Currents at zero and i_q* = I: the first step is K_p I, each of the next 20
// adds K_i T I, v_d stays zero. A gate drop and re-enable, with no new
// selection, starts over.
static void test_foc_integral_ramps_and_restarts_at_an_enable(void)
{
    const float32_t I = 0.5f;   // K_p I + 20 K_i T I = 8 V, under V_lim = 10 V
    const float32_t tolerance = 0.01f * FOC_KP * I;
    float32_t vD = 0.0f;
    float32_t vQ = 0.0f;

    alignAtZero();
    startFocAt(0.0f, I * MAX_VELOCITY / FOC_I_MAX);
    rotorFrameFromCompares(0.0f, &vD, &vQ);
    TEST_ASSERT_FLOAT_WITHIN(tolerance, FOC_KP * I, vQ);
    TEST_ASSERT_FLOAT_WITHIN(tolerance, 0.0f, vD);
    for (uint32_t n = 1U; n <= 20U; n++)
    {
        app_motorControl_run50us();
        rotorFrameFromCompares(0.0f, &vD, &vQ);
        TEST_ASSERT_FLOAT_WITHIN(tolerance, (FOC_KP * I) + ((float32_t)n * FOC_KI_T * I), vQ);
        TEST_ASSERT_FLOAT_WITHIN(tolerance, 0.0f, vD);
    }

    mock_dev_gateDriver_setOperational(GD_MAIN, false);
    tick();
    TEST_ASSERT_FALSE(bridgeEnabled());
    mock_dev_gateDriver_setOperational(GD_MAIN, true);
    app_motorControl_setVelocity(MOTOR_MC, I * MAX_VELOCITY / FOC_I_MAX, 0.0f);   // dark dropped the torque request (fw~mc_026)
    tick();
    advanceTime_ms(PRECHARGE_ADVANCE_MS);
    tick();                                   // precharged: the first step again
    rotorFrameFromCompares(0.0f, &vD, &vQ);
    TEST_ASSERT_FLOAT_WITHIN(tolerance, FOC_KP * I, vQ);
}

// [test->fw~mc_024~1]
// K_p I above V_lim: every step holds magnitude V_lim on the q axis, and the step
// after i_q* returns to zero commands V_lim - K_p I, the back-calculated integral
// alone. A d-axis current turns the command off the q axis; the limit keeps that angle.
static void test_foc_limit_holds_the_vector_and_sets_the_integral(void)
{
    const float32_t vLim = 4.0f;   // V_max under V_bus / sqrt(3)
    const float32_t I = 0.75f;     // K_p I = 6 V: between V_lim and 2 V_lim
    const float32_t tolerance = 0.01f * vLim;
    float32_t vD = 0.0f;
    float32_t vQ = 0.0f;

    appCfg[APP_MOTORCONTROL_CHANNEL_MAIN].maxVoltage_V = vLim;
    TEST_ASSERT_TRUE(app_motorControl_init(&appConfig));
    alignAtZero();
    startFocAt(0.0f, I * MAX_VELOCITY / FOC_I_MAX);
    for (uint32_t n = 0U; n < 20U; n++)
    {
        rotorFrameFromCompares(0.0f, &vD, &vQ);
        TEST_ASSERT_FLOAT_WITHIN(tolerance, vLim, vQ);
        TEST_ASSERT_FLOAT_WITHIN(tolerance, 0.0f, vD);
        app_motorControl_run50us();
    }

    startFocAt(0.0f, I * MAX_VELOCITY / FOC_I_MAX);   // one limited step
    app_motorControl_setVelocity(MOTOR_MC, 0.0f, 0.0f);
    app_motorControl_run1ms();                        // publishes the zero target
    app_motorControl_run50us();
    rotorFrameFromCompares(0.0f, &vD, &vQ);
    TEST_ASSERT_FLOAT_WITHIN(tolerance, vLim - (FOC_KP * I), vQ);

    // I_d = 0.3 A at rotor angle 0 (U = 0.3, V = W = -0.15): e_d = -0.3, e_q = I, so
    // the unlimited command (-2.4, 6) V scales onto V_lim with v_d / v_q = -0.4.
    setPhaseCurrentsUV(0.3f, -0.15f);
    startFocAt(0.0f, I * MAX_VELOCITY / FOC_I_MAX);
    rotorFrameFromCompares(0.0f, &vD, &vQ);
    TEST_ASSERT_FLOAT_WITHIN(tolerance, vLim, sqrtf((vD * vD) + (vQ * vQ)));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -0.4f, vD / vQ);
}

/* ---- the targets in effect: rate limits and the enable seed (fw~mc_027), the torque target (fw~mc_026) ---- */

static float32_t motorTorqueTarget(void)
{
    app_motorControl_snapshot_S s = { 0 };
    (void)app_motorControl_getSnapshot(MOTOR_MC, &s);
    return s.torqueTarget_Nm;
}

// Driving under the torque method at rotor angle 0, both targets at zero.
static void driveInTorqueMode(void)
{
    alignAtZero();
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_FOC_TORQUE);
    app_motorControl_setVelocity(MOTOR_MC, 0.0f, 0.0f);
    tick();
}

// [test->fw~mc_027~1]
// 10 rad/s at 20 rad/s^2 from a standing zero: 0.02 rad/s every pass, 5 rad/s
// at pass 250, 10 rad/s at pass 500 and held there.
static void test_speed_target_ramps_at_its_rate_limit(void)
{
    driveToRunning(0.0f);
    app_motorControl_setVelocity(MOTOR_MC, 10.0f, 20.0f);
    float32_t last = motorSetpoint();
    for (uint32_t pass = 1U; pass <= 500U; pass++)
    {
        tick();
        TEST_ASSERT_FLOAT_WITHIN(0.0002f, 0.02f, motorSetpoint() - last);
        last = motorSetpoint();
        if (pass == 250U) { TEST_ASSERT_FLOAT_WITHIN(0.05f, 5.0f, last); }
    }
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 10.0f, motorSetpoint());
    tick();
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 10.0f, motorSetpoint());
}

// [test->fw~mc_027~1]
// A zero limit steps the torque target to its value on the next pass, and the
// torque method's first step on it commands K_p T / K_t.
static void test_torque_target_steps_at_a_zero_limit(void)
{
    driveInTorqueMode();
    app_motorControl_setTorque(MOTOR_MC, 0.3f, 0.0f);
    tick();
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.3f, motorTorqueTarget());
    float32_t vD = 0.0f;
    float32_t vQ = 0.0f;
    rotorFrameFromCompares(0.0f, &vD, &vQ);
    TEST_ASSERT_FLOAT_WITHIN(0.01f * FOC_KP * FOC_I_MAX, FOC_KP * 0.3f / FOC_KT, vQ);
    TEST_ASSERT_FLOAT_WITHIN(0.01f * FOC_KP * FOC_I_MAX, 0.0f, vD);
}

// [test->fw~mc_027~1]
// An enable with the rotor turning at 5 rad/s: the speed target in effect is
// 5 rad/s on the enable pass and the torque target zero, each then moving at
// its limit from there.
static void test_enable_seeds_speed_from_the_estimate_and_torque_from_zero(void)
{
    mock_dev_gateDriver_setOperational(GD_MAIN, true);
    mock_app_rotorEstimation_setVelocity(5.0f);
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_FOC_TORQUE);
    app_motorControl_setVelocity(MOTOR_MC, 10.0f, 20.0f);   // and, under the torque method, 0.25 Nm at 0.5 Nm/s
    tick();                                                  // the enable pass
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 5.0f, motorSetpoint());
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, motorTorqueTarget());
    tick();
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 5.02f, motorSetpoint());
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0005f, motorTorqueTarget());
}

// [test->fw~mc_026~1]
// The torque target takes its last writer: a torque setpoint, or a speed target
// under the torque method as its fraction of K_t I_max; a setpoint past
// K_t I_max saturates and a non-finite one to zero; another method's speed
// target leaves it.
static void test_torque_target_takes_the_last_writer_and_saturates(void)
{
    driveInTorqueMode();
    app_motorControl_setTorque(MOTOR_MC, 0.4f, 0.0f);
    app_motorControl_setVelocity(MOTOR_MC, MAX_VELOCITY / 2.0f, 0.0f);
    tick();
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, FOC_KT * FOC_I_MAX / 2.0f, motorTorqueTarget());

    app_motorControl_setVelocity(MOTOR_MC, MAX_VELOCITY / 2.0f, 0.0f);
    app_motorControl_setTorque(MOTOR_MC, 0.4f, 0.0f);
    tick();
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.4f, motorTorqueTarget());

    app_motorControl_setTorque(MOTOR_MC, 2.0f * FOC_KT * FOC_I_MAX, 0.0f);
    tick();
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, FOC_KT * FOC_I_MAX, motorTorqueTarget());
    app_motorControl_setTorque(MOTOR_MC, -2.0f * FOC_KT * FOC_I_MAX, 0.0f);
    tick();
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, -FOC_KT * FOC_I_MAX, motorTorqueTarget());
    app_motorControl_setTorque(MOTOR_MC, NAN, 0.0f);
    tick();
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, motorTorqueTarget());

    app_motorControl_setTorque(MOTOR_MC, -FOC_KT * FOC_I_MAX, 0.0f);
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_SINUSOIDAL);
    app_motorControl_setVelocity(MOTOR_MC, MAX_VELOCITY, 0.0f);
    tick();
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, -FOC_KT * FOC_I_MAX, motorTorqueTarget());
}

// [test->fw~mc_026~1]
// A speed target of w_max / 2 at w_max / 2 rad/s^2 under the torque method
// carries a limit of K_t I_max / 2 Nm/s: the torque target is at K_t I_max / 4
// after 500 ms and K_t I_max / 2 after 1 s.
static void test_speed_target_under_the_torque_method_scales_its_limit(void)
{
    driveInTorqueMode();
    app_motorControl_setVelocity(MOTOR_MC, MAX_VELOCITY / 2.0f, MAX_VELOCITY / 2.0f);
    for (uint32_t n = 0U; n < 500U; n++) { tick(); }
    TEST_ASSERT_FLOAT_WITHIN(0.01f * FOC_KT * FOC_I_MAX, FOC_KT * FOC_I_MAX / 4.0f, motorTorqueTarget());
    for (uint32_t n = 0U; n < 500U; n++) { tick(); }
    TEST_ASSERT_FLOAT_WITHIN(0.01f * FOC_KT * FOC_I_MAX, FOC_KT * FOC_I_MAX / 2.0f, motorTorqueTarget());
}

// [test->fw~mc_026~1]
// A written torque does not survive dark: after a disable and after a latched
// fault, the re-enable with no further write leaves the target at zero.
static void test_torque_request_is_cleared_by_disable_and_fault(void)
{
    driveInTorqueMode();
    app_motorControl_setTorque(MOTOR_MC, 0.4f, 0.0f);
    tick();
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.4f, motorTorqueTarget());

    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_OFF);
    tick();
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_FOC_TORQUE);
    tick();
    advanceTime_ms(PRECHARGE_ADVANCE_MS);
    tick();
    tick();
    TEST_ASSERT_TRUE(bridgeEnabled());
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, motorTorqueTarget());

    app_motorControl_setTorque(MOTOR_MC, 0.4f, 0.0f);
    tick();
    setPhaseCurrentsUV(3.0f, 0.0f);
    tick();
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_STATE_FAULTED, motorState());
    setNominalCurrents();
    app_motorControl_clearFault(MOTOR_MC);
    tick();
    advanceTime_ms(PRECHARGE_ADVANCE_MS);
    tick();
    tick();
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_STATE_ENABLED, motorState());
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, motorTorqueTarget());
}

// [test->fw~conn_server_008~1]
// The ratings are the channel's configured constants, the torque ceiling
// their product.
static void test_ratings_are_the_configured_constants(void)
{
    app_motorControl_ratings_S ratings = { 0 };
    TEST_ASSERT_TRUE(app_motorControl_getRatings(MOTOR_MC, &ratings));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, FOC_KT, ratings.torqueConstant_NmPerA);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, FOC_I_MAX, ratings.maxCurrent_A);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, FOC_KT * FOC_I_MAX, ratings.maxTorque_Nm);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, MAX_VELOCITY, ratings.maxVelocity_radPerSec);
    TEST_ASSERT_FALSE(app_motorControl_getRatings(MOTOR_MC, NULL));
    TEST_ASSERT_FALSE(app_motorControl_getRatings(APP_MOTORCONTROL_CHANNEL_COUNT, &ratings));
}

/* ---- method selection ---- */

static app_motorControl_mode_E motorModeNow(void)
{
    app_motorControl_snapshot_S snap = { 0 };
    (void)app_motorControl_getSnapshot(MOTOR_MC, &snap);
    return snap.mode;
}

static app_motorControl_mode_E selectedMode(void)
{
    app_motorControl_snapshot_S snap = { 0 };
    (void)app_motorControl_getSnapshot(MOTOR_MC, &snap);
    return snap.selectedMode;
}

// A method request selects it; OFF disables and leaves the selection, so the
// next enable runs the same method. The default comes from the config.
static void test_selected_method_survives_off(void)
{
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_MODE_SIX_STEP_TRAP, selectedMode());   // config default
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_SINUSOIDAL);
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_MODE_SINUSOIDAL, selectedMode());
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_OFF);
    tick();
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_MODE_OFF, motorModeNow());
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_MODE_SINUSOIDAL, selectedMode());
}

static void test_init_rejects_off_default_mode(void)
{
    buildConfigs();
    appCfg[APP_MOTORCONTROL_CHANNEL_MAIN].defaultMode = APP_MOTORCONTROL_MODE_OFF;
    TEST_ASSERT_FALSE(app_motorControl_init(&appConfig));
}

/* ---- fw~mc_019: the per-cycle rotor angle ---- */

static float32_t motorAngle_rad(void)
{
    app_motorControl_snapshot_S snap = { 0 };
    (void)app_motorControl_getSnapshot(MOTOR_MC, &snap);
    return snap.magneticAngle_rad;
}

// [test->fw~mc_019~1]
// Every cycle's electrical angle is the estimator's mechanical angle for that
// cycle, less the alignment offset, times the pole pairs, wrapped: the step
// follows the estimate cycle by cycle with no 1 ms pass in between, and the
// offset carries a reading below it round through 2 pi.
static void test_cycle_step_follows_the_estimate_every_cycle(void)
{
    alignAtZero();                                    // offset 0, pole pairs 1
    mock_app_rotorEstimation_setVelocity(21.0f);      // a velocity the old law would extrapolate on

    mock_app_rotorEstimation_setAngle(1.0f);
    app_motorControl_run50us();
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, motorAngle_rad());

    mock_app_rotorEstimation_setAngle(1.001f);        // the estimate advanced one cycle
    app_motorControl_run50us();
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.001f, motorAngle_rad());
}

// [test->fw~mc_019~1]
// With the offset captured at 2 rad, an estimate of 0.5 rad reads 2 pi - 1.5:
// the subtraction wraps into [0, 2 pi).
static void test_cycle_step_wraps_the_offset_angle(void)
{
    mock_dev_gateDriver_setOperational(GD_MAIN, true);
    mock_app_rotorEstimation_setAngle(2.0f);
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_SIX_STEP_TRAP);
    app_motorControl_setVelocity(MOTOR_MC, 10.0f, 0.0f);
    tick();
    advanceTime_ms(PRECHARGE_ADVANCE_MS);
    tick();
    advanceTime_ms(ALIGN_ADVANCE_MS);
    tick();                                           // dwell expires, offset := 2.0

    mock_app_rotorEstimation_setVelocity(21.0f);
    mock_app_rotorEstimation_setAngle(0.5f);
    app_motorControl_run50us();
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 6.2831853f - 1.5f, motorAngle_rad());
}

/* ---- fw~mc_021: the bootstrap precharge on every enable ---- */

static bool allPhasesAtZeroDuty(void)
{
    return (phaseCompare(IO_BRIDGE_PHASE_U) == 0U) &&
           (phaseCompare(IO_BRIDGE_PHASE_V) == 0U) &&
           (phaseCompare(IO_BRIDGE_PHASE_W) == 0U);
}

// [test->fw~mc_021~1]
// The first enable holds the three low sides on for the precharge time, the
// bridge live, before the alignment pattern starts.
static void test_first_enable_precharges_before_alignment(void)
{
    mock_dev_gateDriver_setOperational(GD_MAIN, true);
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_SIX_STEP_TRAP);
    app_motorControl_setVelocity(MOTOR_MC, 10.0f, 0.0f);

    tick();
    TEST_ASSERT_TRUE(bridgeEnabled());
    TEST_ASSERT_TRUE(allPhasesAtZeroDuty());
    advanceTime_ms(PRECHARGE_MS - 2U);
    tick();
    TEST_ASSERT_TRUE(allPhasesAtZeroDuty());            // still charging

    advanceTime_ms(3U);
    tick();
    TEST_ASSERT_EQUAL_UINT32(DUTY_COMPARE(ALIGN_DUTY_01), phaseCompare(IO_BRIDGE_PHASE_U));   // alignment begins
}

// [test->fw~mc_021~1]
// A later enable, the rotor already aligned, precharges again and then drives
// without re-aligning.
static void test_re_enable_precharges_then_drives(void)
{
    driveToRunning(10.0f);
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_OFF);
    tick();
    TEST_ASSERT_FALSE(bridgeEnabled());

    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_SIX_STEP_TRAP);
    tick();
    TEST_ASSERT_TRUE(bridgeEnabled());
    TEST_ASSERT_TRUE(allPhasesAtZeroDuty());

    advanceTime_ms(PRECHARGE_ADVANCE_MS);
    tick();
    TEST_ASSERT_FALSE(allPhasesAtZeroDuty());           // commutating, not aligning
    TEST_ASSERT_NOT_EQUAL(DUTY_COMPARE(ALIGN_DUTY_01), phaseCompare(IO_BRIDGE_PHASE_U));
}

/* ---- fw~mc_015: the step owns the outputs ---- */

// [test->fw~mc_015~1]
// A new angle and target published by the 1 ms pass reach the bridge only when
// the cycle step runs: the pass alone leaves the compares where the last step
// put them, the step then lands the commutation for the published inputs.
static void test_cycle_step_not_the_ms_pass_writes_the_outputs(void)
{
    alignAtZero();                                       // aligned; last step applied the alignment pattern
    TEST_ASSERT_EQUAL_UINT32(DUTY_COMPARE(ALIGN_DUTY_01), drivenCompare());

    mock_app_rotorEstimation_setAngle(DEG2RAD(90.0f));
    app_motorControl_setVelocity(MOTOR_MC, 10.0f, 0.0f);
    app_motorControl_run1ms();                           // publishes angle + target, decides Driving
    TEST_ASSERT_EQUAL_UINT32(DUTY_COMPARE(ALIGN_DUTY_01), drivenCompare());   // outputs untouched

    app_motorControl_run50us();                        // the step commutates on them
    TEST_ASSERT_EQUAL_UINT32(DUTY_COMPARE(0.5f), drivenCompare());
    TEST_ASSERT_TRUE(bridgeEnabled());
}

/* ---- fw~mc_012: alignment offset capture ---- */

// [test->fw~mc_012~1]
static void test_first_enable_aligns_then_captures_offset(void)
{
    // Enable at a non-zero rotor angle so the captured offset is observable.
    mock_dev_gateDriver_setOperational(GD_MAIN, true);
    mock_app_rotorEstimation_setAngle(DEG2RAD(45.0f));
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_SIX_STEP_TRAP);
    app_motorControl_setVelocity(MOTOR_MC, 10.0f, 0.0f);

    // The precharge comes first on every enable (fw~mc_021).
    tick();
    advanceTime_ms(PRECHARGE_ADVANCE_MS);

    // Dwell cycle 1: alignment pattern (U at align duty, V and W returning at
    // zero duty), not yet aligned.
    tick();
    app_motorControl_snapshot_S snap = { 0 };
    (void)app_motorControl_getSnapshot(MOTOR_MC, &snap);
    TEST_ASSERT_FALSE(snap.isAligned);
    TEST_ASSERT_TRUE(bridgeEnabled());
    TEST_ASSERT_EQUAL_UINT32(DUTY_COMPARE(ALIGN_DUTY_01), phaseCompare(IO_BRIDGE_PHASE_U));
    TEST_ASSERT_EQUAL_UINT32(0U, phaseCompare(IO_BRIDGE_PHASE_V));
    TEST_ASSERT_EQUAL_UINT32(0U, phaseCompare(IO_BRIDGE_PHASE_W));
    TEST_ASSERT_FALSE(phaseFloating(IO_BRIDGE_PHASE_U));
    TEST_ASSERT_FALSE(phaseFloating(IO_BRIDGE_PHASE_V));
    TEST_ASSERT_FALSE(phaseFloating(IO_BRIDGE_PHASE_W));

    // Still aligning partway through the dwell.
    advanceTime_ms(200U);
    tick();
    (void)app_motorControl_getSnapshot(MOTOR_MC, &snap);
    TEST_ASSERT_FALSE(snap.isAligned);

    // After the dwell the offset is captured; the following cycle commutates on
    // it, so the electrical angle (rotor 45deg minus offset 45deg) reads ~0.
    advanceTime_ms(ALIGN_ADVANCE_MS);
    tick();
    (void)app_motorControl_getSnapshot(MOTOR_MC, &snap);
    TEST_ASSERT_TRUE(snap.isAligned);

    tick();
    (void)app_motorControl_getSnapshot(MOTOR_MC, &snap);
    TEST_ASSERT_FLOAT_WITHIN(0.02f, 0.0f, snap.magneticAngle_rad);
}

// [test->fw~mc_012~1]
// A subsequent enable commutates immediately on the stored offset — no second
// alignment dwell.
static void test_subsequent_enable_skips_alignment(void)
{
    alignAtZero();                 // aligned, offset 0
    tick();     // commutating

    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_OFF);
    tick();
    TEST_ASSERT_FALSE(bridgeEnabled());

    // Re-enable: after the precharge (fw~mc_021) the next cycle commutates at
    // the drive duty (0.5), not back at the alignment duty (0.1).
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_SIX_STEP_TRAP);
    tick();          // precharging
    advanceTime_ms(PRECHARGE_ADVANCE_MS);
    commutateAt(90.0f, 10.0f);

    app_motorControl_snapshot_S snap = { 0 };
    (void)app_motorControl_getSnapshot(MOTOR_MC, &snap);
    TEST_ASSERT_TRUE(snap.isAligned);
    TEST_ASSERT_EQUAL_UINT32(DUTY_COMPARE(0.5f), drivenCompare());
}

/* ---- init config validation ---- */

static void test_init_rejects_zero_pole_pairs(void)
{
    buildConfigs();
    appCfg[APP_MOTORCONTROL_CHANNEL_MAIN].motorPolePairs = 0U;
    TEST_ASSERT_FALSE(app_motorControl_init(&appConfig));
}

static void test_init_rejects_zero_max_velocity(void)
{
    buildConfigs();
    appCfg[APP_MOTORCONTROL_CHANNEL_MAIN].maxVelocity_radPerSec = 0.0f;
    TEST_ASSERT_FALSE(app_motorControl_init(&appConfig));
}

static void test_init_rejects_zero_pwm_period(void)
{
    buildConfigs();
    appCfg[APP_MOTORCONTROL_CHANNEL_MAIN].pwmPeriod_s = 0.0f;
    TEST_ASSERT_FALSE(app_motorControl_init(&appConfig));
}

static void test_init_rejects_zero_max_voltage(void)
{
    buildConfigs();
    appCfg[APP_MOTORCONTROL_CHANNEL_MAIN].maxVoltage_V = 0.0f;
    TEST_ASSERT_FALSE(app_motorControl_init(&appConfig));
}

// [test->fw~mc_022~1]
static void test_init_rejects_a_current_limit_at_the_phase_trip(void)
{
    buildConfigs();
    appCfg[APP_MOTORCONTROL_CHANNEL_MAIN].foc.maxCurrent_A = PHASE_TRIP_A;
    TEST_ASSERT_FALSE(app_motorControl_init(&appConfig));
    appCfg[APP_MOTORCONTROL_CHANNEL_MAIN].foc.maxCurrent_A = 0.0f;
    TEST_ASSERT_FALSE(app_motorControl_init(&appConfig));
}

// [test->fw~mc_023~1]
// R, L, or w_bw at zero is rejected; w_bw T is accepted at 0.19 and rejected at 0.21.
static void test_init_rejects_a_degenerate_current_loop(void)
{
    buildConfigs();
    appCfg[APP_MOTORCONTROL_CHANNEL_MAIN].foc.motorR_ohm = 0.0f;
    TEST_ASSERT_FALSE(app_motorControl_init(&appConfig));
    buildConfigs();
    appCfg[APP_MOTORCONTROL_CHANNEL_MAIN].foc.motorL_H = 0.0f;
    TEST_ASSERT_FALSE(app_motorControl_init(&appConfig));
    buildConfigs();
    appCfg[APP_MOTORCONTROL_CHANNEL_MAIN].foc.currentLoopBandwidth_radPerSec = 0.0f;
    TEST_ASSERT_FALSE(app_motorControl_init(&appConfig));

    const float32_t period_s = appCfg[APP_MOTORCONTROL_CHANNEL_MAIN].pwmPeriod_s;
    buildConfigs();
    appCfg[APP_MOTORCONTROL_CHANNEL_MAIN].foc.currentLoopBandwidth_radPerSec = 0.19f / period_s;
    TEST_ASSERT_TRUE(app_motorControl_init(&appConfig));
    appCfg[APP_MOTORCONTROL_CHANNEL_MAIN].foc.currentLoopBandwidth_radPerSec = 0.21f / period_s;
    TEST_ASSERT_FALSE(app_motorControl_init(&appConfig));
}

/* ---- fw~mc_029..032: the position method, its setpoint, the move profile, the rotor position ---- */

#define TWO_PI_F        (6.28318530718f)
#define RAD2DEG(r)      ((float32_t)(r) * 180.0f / 3.14159265f)
#define POS_KPP         (30.0f)         // the test config's position gains (mirror buildConfigs)
#define POS_MAX         (20.0f * 3.14159265f)

static float32_t motorPositionTarget(void)
{
    app_motorControl_snapshot_S s = { 0 };
    (void)app_motorControl_getSnapshot(MOTOR_MC, &s);
    return s.positionTarget_rad;
}

static float32_t motorProfileVelocity(void)
{
    app_motorControl_snapshot_S s = { 0 };
    (void)app_motorControl_getSnapshot(MOTOR_MC, &s);
    return s.profileVelocity_radPerSec;
}

static float32_t motorRotorPosition(void)
{
    app_motorControl_snapshot_S s = { 0 };
    (void)app_motorControl_getSnapshot(MOTOR_MC, &s);
    return s.rotorPosition_rad;
}

// The position method driving at rest: aligned at zero, selected, its first step
// run on a zero target with nothing written since the enable.
static void driveInPositionMode(void)
{
    alignAtZero();
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_FOC_POSITION);
    tick();
    TEST_ASSERT_TRUE(bridgeEnabled());
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, motorPositionTarget());
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, motorProfileVelocity());
}

// Passes until theta* rests on target (w_f zero, within 1e-4 rad), at most maxPasses.
// The peak |w_f| and the largest theta* seen come back through the optional pointers.
static uint32_t runToRest(float32_t target, uint32_t maxPasses, float32_t * const peakSpeed, float32_t * const maxTarget)
{
    uint32_t passes = 0U;
    bool atRest = false;
    float32_t peak = 0.0f;
    float32_t largest = motorPositionTarget();
    while ((!atRest) && (passes < maxPasses))
    {
        tick();
        passes++;
        const float32_t w = motorProfileVelocity();
        const float32_t t = motorPositionTarget();
        if (fabsf(w) > peak) { peak = fabsf(w); }
        if (t > largest) { largest = t; }
        atRest = ((fabsf(w) < 1e-6f) && (fabsf(t - target) < 1e-4f));
    }
    TEST_ASSERT_TRUE_MESSAGE(atRest, "the profile did not come to rest on the target");
    if (peakSpeed != NULL) { *peakSpeed = peak; }
    if (maxTarget != NULL) { *maxTarget = largest; }
    return passes;
}

// Passes until theta* first reaches target, at most maxPasses.
static uint32_t runToReach(float32_t target, uint32_t maxPasses)
{
    uint32_t passes = 0U;
    bool reached = false;
    while ((!reached) && (passes < maxPasses))
    {
        tick();
        passes++;
        reached = (motorPositionTarget() >= (target - 1e-6f));
    }
    TEST_ASSERT_TRUE_MESSAGE(reached, "the target was not reached");
    return passes;
}

// The bridge dark, then the position method re-enabled through its precharge: the
// pass that begins driving (its second cycle is the method's first step).
static void disableThenReenable(void)
{
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_OFF);
    tick();
    TEST_ASSERT_FALSE(bridgeEnabled());
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_FOC_POSITION);
    tick();                                   // precharging
    advanceTime_ms(PRECHARGE_ADVANCE_MS);
    tick();                                   // driving: the first step
    TEST_ASSERT_TRUE(bridgeEnabled());
}

// [test->fw~mc_031~1]
// 2 pi at 10 rad/s, 50 rad/s^2, stopping: at the limit 200 ms in, at rest on 2 pi
// at 828 ms, never past it.
static void test_profile_trapezoid_to_two_pi(void)
{
    driveInPositionMode();
    app_motorControl_setPositionSetpoint(MOTOR_MC, TWO_PI_F, 10.0f, 50.0f, 0.0f);
    for (uint32_t n = 0U; n < 200U; n++) { tick(); }
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 10.0f, motorProfileVelocity());
    float32_t largest = 0.0f;
    const uint32_t passes = 200U + runToRest(TWO_PI_F, 2000U, NULL, &largest);
    TEST_ASSERT_UINT32_WITHIN(2U, 828U, passes);
    TEST_ASSERT_TRUE(largest <= (TWO_PI_F + 1e-5f));
}

// [test->fw~mc_031~1]
// 0.5 rad under the same limits is a triangle: 5 rad/s at 100 ms, at rest at 200 ms.
static void test_profile_triangle_to_half_a_radian(void)
{
    driveInPositionMode();
    app_motorControl_setPositionSetpoint(MOTOR_MC, 0.5f, 10.0f, 50.0f, 0.0f);
    for (uint32_t n = 0U; n < 100U; n++) { tick(); }
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 5.0f, motorProfileVelocity());
    const uint32_t passes = 100U + runToRest(0.5f, 1000U, NULL, NULL);
    TEST_ASSERT_UINT32_WITHIN(2U, 200U, passes);
}

// [test->fw~mc_031~1]
// A zero acceleration limit steps w_f to v on the next pass: 2 pi at 10 rad/s in 628 ms.
static void test_profile_steps_at_a_zero_acceleration_limit(void)
{
    driveInPositionMode();
    app_motorControl_setPositionSetpoint(MOTOR_MC, TWO_PI_F, 10.0f, 0.0f, 0.0f);
    tick();
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 10.0f, motorProfileVelocity());
    const uint32_t passes = 1U + runToRest(TWO_PI_F, 2000U, NULL, NULL);
    TEST_ASSERT_UINT32_WITHIN(2U, 628U, passes);
}

// [test->fw~mc_031~1]
// A zero velocity limit stands for w_max: 4 pi at 50 rad/s^2 reaches it before the point.
static void test_profile_zero_velocity_limit_is_the_maximum_velocity(void)
{
    driveInPositionMode();
    app_motorControl_setPositionSetpoint(MOTOR_MC, 2.0f * TWO_PI_F, 0.0f, 50.0f, 0.0f);
    uint32_t passes = 0U;
    while ((motorProfileVelocity() < (0.99f * MAX_VELOCITY)) && (passes < 2000U)) { tick(); passes++; }
    TEST_ASSERT_FLOAT_WITHIN(0.01f * MAX_VELOCITY, MAX_VELOCITY, motorProfileVelocity());
    TEST_ASSERT_TRUE(motorPositionTarget() < (2.0f * TWO_PI_F));
    (void)runToRest(2.0f * TWO_PI_F, 2000U, NULL, NULL);
}

// [test->fw~mc_031~1]
// A velocity at the point of 10 rad/s: 2 pi is reached at 728 ms still at 10 rad/s and
// w_f holds past it; 4 pi stopping, written at that moment, is reached 728 ms later.
static void test_profile_arrives_at_the_velocity_at_the_point_and_chains(void)
{
    driveInPositionMode();
    app_motorControl_setPositionSetpoint(MOTOR_MC, TWO_PI_F, 10.0f, 50.0f, 10.0f);
    const uint32_t passes = runToReach(TWO_PI_F, 2000U);
    TEST_ASSERT_UINT32_WITHIN(2U, 728U, passes);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 10.0f, motorProfileVelocity());

    app_motorControl_setPositionSetpoint(MOTOR_MC, 2.0f * TWO_PI_F, 10.0f, 50.0f, 0.0f);
    const uint32_t chained = runToRest(2.0f * TWO_PI_F, 2000U, NULL, NULL);
    TEST_ASSERT_UINT32_WITHIN(2U, 728U, chained);
}

// [test->fw~mc_031~1]
// Past the point w_f holds at w_s: 100 ms later theta* has moved a further radian, and
// 4 pi stopping written then is reached 628 ms later.
static void test_profile_holds_the_velocity_past_the_point(void)
{
    driveInPositionMode();
    app_motorControl_setPositionSetpoint(MOTOR_MC, TWO_PI_F, 10.0f, 50.0f, 10.0f);
    (void)runToReach(TWO_PI_F, 2000U);
    const float32_t atPoint = motorPositionTarget();
    for (uint32_t n = 0U; n < 100U; n++)
    {
        tick();
        TEST_ASSERT_FLOAT_WITHIN(1e-3f, 10.0f, motorProfileVelocity());
    }
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.0f, motorPositionTarget() - atPoint);

    app_motorControl_setPositionSetpoint(MOTOR_MC, 2.0f * TWO_PI_F, 10.0f, 50.0f, 0.0f);
    const uint32_t late = runToRest(2.0f * TWO_PI_F, 2000U, NULL, NULL);
    TEST_ASSERT_UINT32_WITHIN(2U, 628U, late);
}

// [test->fw~mc_031~1]
// Rewritten to 0 while cruising at 10 rad/s toward 2 pi: w_f falls 0.05 rad/s a pass
// through zero, and theta* comes back to rest on 0.
static void test_profile_reverses_at_the_acceleration_limit(void)
{
    driveInPositionMode();
    app_motorControl_setPositionSetpoint(MOTOR_MC, TWO_PI_F, 10.0f, 50.0f, 0.0f);
    for (uint32_t n = 0U; n < 300U; n++) { tick(); }
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 10.0f, motorProfileVelocity());

    app_motorControl_setPositionSetpoint(MOTOR_MC, 0.0f, 10.0f, 50.0f, 0.0f);
    float32_t last = motorProfileVelocity();
    bool crossedZero = false;
    for (uint32_t n = 0U; n < 200U; n++)
    {
        tick();
        TEST_ASSERT_FLOAT_WITHIN(1e-4f, -0.05f, motorProfileVelocity() - last);
        last = motorProfileVelocity();
        if (last < 0.0f) { crossedZero = true; }
    }
    TEST_ASSERT_TRUE(crossedZero);
    (void)runToRest(0.0f, 3000U, NULL, NULL);
}

// [test->fw~mc_031~1]
// A speed target under the position method jogs: 2 rad/s steps w_f to 2 and theta* on
// by 0.002 a pass, zero holds, and a position setpoint a radian on from that rest is
// reached in 283 ms.
static void test_speed_target_jogs_the_position_target(void)
{
    driveInPositionMode();
    app_motorControl_setVelocity(MOTOR_MC, 2.0f, 0.0f);
    tick();
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 2.0f, motorProfileVelocity());
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.002f, motorPositionTarget());
    for (uint32_t n = 0U; n < 9U; n++) { tick(); }
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.02f, motorPositionTarget());

    app_motorControl_setVelocity(MOTOR_MC, 0.0f, 0.0f);
    tick();
    const float32_t held = motorPositionTarget();
    for (uint32_t n = 0U; n < 10U; n++) { tick(); }
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, held, motorPositionTarget());
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, motorProfileVelocity());

    app_motorControl_setPositionSetpoint(MOTOR_MC, held + 1.0f, 10.0f, 50.0f, 0.0f);
    const uint32_t passes = runToRest(held + 1.0f, 1000U, NULL, NULL);
    TEST_ASSERT_UINT32_WITHIN(2U, 283U, passes);
}

// [test->fw~mc_031~1]
// A jog at w_max stops theta* on theta_max with w_f zero; a jog of -w_max moves it off.
static void test_profile_stops_at_the_travel_bound(void)
{
    driveInPositionMode();
    app_motorControl_setVelocity(MOTOR_MC, MAX_VELOCITY, 0.0f);
    (void)runToReach(POS_MAX, 5000U);
    tick();
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, POS_MAX, motorPositionTarget());
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, motorProfileVelocity());

    app_motorControl_setVelocity(MOTOR_MC, -MAX_VELOCITY, 0.0f);
    tick();
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, -MAX_VELOCITY, motorProfileVelocity());
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, POS_MAX - (MAX_VELOCITY * 1e-3f), motorPositionTarget());
}

// [test->fw~mc_031~1] [test->fw~mc_030~1]
// 1 rad written during the precharge of a re-enable is held: theta* is zero until the
// method's first step and rests on 1 rad 283 ms after it.
static void test_setpoint_written_in_precharge_runs_from_the_first_step(void)
{
    driveInPositionMode();
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_OFF);
    tick();
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_FOC_POSITION);
    tick();                                   // precharging
    app_motorControl_setPositionSetpoint(MOTOR_MC, 1.0f, 10.0f, 50.0f, 0.0f);
    tick();
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, motorPositionTarget());
    advanceTime_ms(PRECHARGE_ADVANCE_MS);
    tick();                                   // driving: the first step
    const uint32_t passes = 1U + runToRest(1.0f, 1000U, NULL, NULL);
    TEST_ASSERT_UINT32_WITHIN(2U, 283U, passes);
}

// [test->fw~mc_030~1]
// Two position setpoints in one pass leave the second held: theta* rests on it.
static void test_position_setpoint_takes_the_last_write(void)
{
    driveInPositionMode();
    app_motorControl_setPositionSetpoint(MOTOR_MC, 1.0f, 10.0f, 50.0f, 0.0f);
    app_motorControl_setPositionSetpoint(MOTOR_MC, 2.0f, 10.0f, 50.0f, 0.0f);
    float32_t largest = 0.0f;
    (void)runToRest(2.0f, 1000U, NULL, &largest);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 2.0f, largest);
}

// [test->fw~mc_030~1]
// A setpoint written while driving does not survive a disable: the re-enable with no
// further write leaves theta* at zero.
static void test_position_setpoint_is_dropped_by_a_disable(void)
{
    driveInPositionMode();
    app_motorControl_setPositionSetpoint(MOTOR_MC, 2.0f, 10.0f, 50.0f, 0.0f);
    tick();
    TEST_ASSERT_TRUE(motorProfileVelocity() > 0.0f);
    disableThenReenable();
    for (uint32_t n = 0U; n < 100U; n++) { tick(); }
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, motorPositionTarget());
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, motorProfileVelocity());
}

// [test->fw~mc_030~1]
// A setpoint written while the bridge is dark is held at the following enable.
static void test_position_setpoint_written_while_dark_is_held_to_the_enable(void)
{
    driveInPositionMode();
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_OFF);
    tick();
    tick();
    app_motorControl_setPositionSetpoint(MOTOR_MC, 1.0f, 10.0f, 50.0f, 0.0f);
    tick();
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_FOC_POSITION);
    tick();
    advanceTime_ms(PRECHARGE_ADVANCE_MS);
    tick();
    const uint32_t passes = 1U + runToRest(1.0f, 1000U, NULL, NULL);
    TEST_ASSERT_UINT32_WITHIN(2U, 283U, passes);
}

// [test->fw~mc_032~1]
// The estimator's angle stepped across the seam, 6.2 to 0.1 rad and back, moves the
// rotor position by +0.1832 then -0.1832 rad.
static void test_rotor_position_follows_the_angle_across_the_seam(void)
{
    driveInPositionMode();
    mock_app_rotorEstimation_setAngle(6.2f);
    tick();
    const float32_t before = motorRotorPosition();
    mock_app_rotorEstimation_setAngle(0.1f);
    tick();
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.1832f, motorRotorPosition() - before);
    mock_app_rotorEstimation_setAngle(6.2f);
    tick();
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, before, motorRotorPosition());
}

// Step the estimator's angle by quarter turns, `steps` of them, forward or back.
static void turnRotor(int32_t steps)
{
    static int32_t quarterTurns = 0;
    const int32_t dir = (steps < 0) ? -1 : 1;
    for (int32_t n = 0; n < (dir * steps); n++)
    {
        quarterTurns += dir;
        const float32_t angle = fmodf(fmodf((float32_t)quarterTurns * (TWO_PI_F / 4.0f), TWO_PI_F) + TWO_PI_F, TWO_PI_F);
        mock_app_rotorEstimation_setAngle(angle);
        tick();
    }
}

// [test->fw~mc_032~1]
// Three turns forward and one back read 4 pi; ten forward and ten back read zero; the
// position is zero through a disable and the next enable's precharge.
static void test_rotor_position_counts_turns(void)
{
    driveInPositionMode();
    turnRotor(12);
    turnRotor(-4);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 2.0f * TWO_PI_F, motorRotorPosition());
    turnRotor(40);
    turnRotor(-48);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, motorRotorPosition());
    turnRotor(5);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.25f * TWO_PI_F, motorRotorPosition());

    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_OFF);
    tick();
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, motorRotorPosition());
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_FOC_POSITION);
    tick();                                   // precharging
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, motorRotorPosition());
}

// [test->fw~mc_029~1]
// Nothing written, the estimator's angle stepped by -E after the first step: the next
// step commands i_q* = K_vp K_pp E / K_t, read as K_p i_q* on the q axis.
static void test_position_first_step_is_the_proportional_velocity_command(void)
{
    const float32_t E = 0.2f;   // K_pp E = 6 rad/s under w_max, K_vp K_pp E = 0.06 Nm under K_t I_max
    driveInPositionMode();
    mock_app_rotorEstimation_setAngle(-E);
    app_motorControl_run50us();
    float32_t vD = 0.0f;
    float32_t vQ = 0.0f;
    rotorFrameFromCompares(RAD2DEG(-E), &vD, &vQ);
    const float32_t iQ = FOC_KVP * POS_KPP * E / FOC_KT;
    TEST_ASSERT_FLOAT_WITHIN(0.01f * FOC_KP * iQ, FOC_KP * iQ, vQ);
    TEST_ASSERT_FLOAT_WITHIN(0.01f * FOC_KP * iQ, 0.0f, vD);
}

// [test->fw~mc_029~1]
// K_pp E at 1.5 w_max: the step commands w_max, and the step after the angle steps
// back commands zero, the integral held through the clamp. The second readback
// carries the first step's current-loop integral, K_i T times its i_q*.
static void test_position_clamps_the_velocity_command_and_holds_the_integral(void)
{
    const float32_t E = 1.5f * MAX_VELOCITY / POS_KPP;
    driveInPositionMode();
    mock_app_rotorEstimation_setAngle(-E);
    app_motorControl_run50us();
    float32_t vD = 0.0f;
    float32_t vQ = 0.0f;
    rotorFrameFromCompares(RAD2DEG(-E), &vD, &vQ);
    const float32_t iQmax = FOC_KVP * MAX_VELOCITY / FOC_KT;
    TEST_ASSERT_FLOAT_WITHIN(0.01f * FOC_KP * iQmax, FOC_KP * iQmax, vQ);

    mock_app_rotorEstimation_setAngle(0.0f);
    app_motorControl_run50us();
    rotorFrameFromCompares(0.0f, &vD, &vQ);
    TEST_ASSERT_FLOAT_WITHIN(0.01f * FOC_KP * iQmax, FOC_KI_T * iQmax, vQ);
}

// [test->fw~mc_029~1]
// The angle held, 4 rad at v = F stepping: the first step after the next pass commands
// F (1 + K_pp 1 ms), the profile velocity fed forward past the position error it made.
static void test_position_feeds_the_profile_velocity_forward(void)
{
    const float32_t F = 10.0f;
    driveInPositionMode();
    app_motorControl_setPositionSetpoint(MOTOR_MC, 4.0f, F, 0.0f, 0.0f);
    tick();
    float32_t vD = 0.0f;
    float32_t vQ = 0.0f;
    rotorFrameFromCompares(0.0f, &vD, &vQ);
    const float32_t iQ = FOC_KVP * F * (1.0f + (POS_KPP * 1e-3f)) / FOC_KT;
    TEST_ASSERT_FLOAT_WITHIN(0.01f * FOC_KP * FOC_KVP * F / FOC_KT, FOC_KP * iQ, vQ);
}

// [test->fw~mc_029~1] K_pp, K_pi at or below zero rejected; [test->fw~mc_031~1] the travel bound likewise
static void test_init_rejects_a_degenerate_position_loop(void)
{
    buildConfigs();
    appCfg[APP_MOTORCONTROL_CHANNEL_MAIN].foc.positionKp_perSec = 0.0f;
    TEST_ASSERT_FALSE(app_motorControl_init(&appConfig));
    buildConfigs();
    appCfg[APP_MOTORCONTROL_CHANNEL_MAIN].foc.positionKi_perSec2 = -1.0f;
    TEST_ASSERT_FALSE(app_motorControl_init(&appConfig));
    buildConfigs();
    appCfg[APP_MOTORCONTROL_CHANNEL_MAIN].foc.maxPosition_rad = 0.0f;
    TEST_ASSERT_FALSE(app_motorControl_init(&appConfig));
}

// [test->fw~mc_025~1]
static void test_init_rejects_a_degenerate_velocity_loop(void)
{
    buildConfigs();
    appCfg[APP_MOTORCONTROL_CHANNEL_MAIN].foc.torqueConstant_NmPerA = 0.0f;
    TEST_ASSERT_FALSE(app_motorControl_init(&appConfig));
    buildConfigs();
    appCfg[APP_MOTORCONTROL_CHANNEL_MAIN].foc.velocityKp_NmPerRadPerSec = 0.0f;
    TEST_ASSERT_FALSE(app_motorControl_init(&appConfig));
    buildConfigs();
    appCfg[APP_MOTORCONTROL_CHANNEL_MAIN].foc.velocityKi_NmPerRad = -1.0f;
    TEST_ASSERT_FALSE(app_motorControl_init(&appConfig));
    // the feed-forward gain is a share, [0, 1]
    static const float32_t rejected[] = { -0.01f, 1.01f, NAN };
    for (size_t i = 0U; i < COUNTOF(rejected); i++)
    {
        buildConfigs();
        appCfg[APP_MOTORCONTROL_CHANNEL_MAIN].foc.velocityKff = rejected[i];
        TEST_ASSERT_FALSE(app_motorControl_init(&appConfig));
    }
    static const float32_t accepted[] = { 0.0f, 1.0f };
    for (size_t i = 0U; i < COUNTOF(accepted); i++)
    {
        buildConfigs();
        appCfg[APP_MOTORCONTROL_CHANNEL_MAIN].foc.velocityKff = accepted[i];
        TEST_ASSERT_TRUE(app_motorControl_init(&appConfig));
    }
}

// [test->fw~mc_028~1]
static void test_init_rejects_a_degenerate_cogging_model(void)
{
    static const float32_t rejected[] = { -0.01f, 1.01f, NAN };
    for (size_t i = 0U; i < COUNTOF(rejected); i++)
    {
        buildConfigs();
        appCfg[APP_MOTORCONTROL_CHANNEL_MAIN].foc.coggingKff = rejected[i];
        TEST_ASSERT_FALSE(app_motorControl_init(&appConfig));
    }
    buildConfigs();
    appCfg[APP_MOTORCONTROL_CHANNEL_MAIN].foc.cogging[1].amplitude_Nm = -1.0e-3f;
    TEST_ASSERT_FALSE(app_motorControl_init(&appConfig));
    buildConfigs();
    appCfg[APP_MOTORCONTROL_CHANNEL_MAIN].foc.coggingKff = 1.0f;
    appCfg[APP_MOTORCONTROL_CHANNEL_MAIN].foc.cogging[0].amplitude_Nm = 0.5f;
    TEST_ASSERT_TRUE(app_motorControl_init(&appConfig));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_drives_bridge_when_operational_and_unfaulted);

    RUN_TEST(test_phase_overcurrent_trips_and_latches);
    RUN_TEST(test_bus_overcurrent_trips);
    RUN_TEST(test_current_below_threshold_does_not_trip);
    RUN_TEST(test_negative_phase_overcurrent_trips);

    RUN_TEST(test_gate_blocks_bridge_when_not_operational);
    RUN_TEST(test_gate_blocks_bridge_while_fault_latched);
    RUN_TEST(test_off_request_disables_bridge_same_cycle);
    RUN_TEST(test_zero_demand_holds_bridge_enabled);

    RUN_TEST(test_state_view_tracks_disabled_enabled_faulted);
    RUN_TEST(test_velocity_setpoint_exposed);
    RUN_TEST(test_velocity_view_is_the_estimators);
    RUN_TEST(test_clear_fault_releases_latch_and_resumes);
    RUN_TEST(test_fault_latches_are_counted);

    RUN_TEST(test_encoder_fault_latches_past_limit);
    RUN_TEST(test_encoder_fault_count_resets_on_valid_read);

    RUN_TEST(test_sector_table_drives_correct_phases);
    RUN_TEST(test_reversal_flips_drive_direction);
    RUN_TEST(test_duty_proportional_and_clamped);

    RUN_TEST(test_sinusoidal_vector_sits_on_the_q_axis);
    RUN_TEST(test_sinusoidal_zero_target_centres_the_duties_with_the_bridge_up);
    RUN_TEST(test_foc_first_step_is_kp_times_the_current_target);
    RUN_TEST(test_foc_integral_ramps_and_restarts_at_an_enable);
    RUN_TEST(test_foc_velocity_feeds_the_disturbance_estimate_forward);
    RUN_TEST(test_foc_velocity_back_calculates_the_integral_net_of_the_feed_forward);
    RUN_TEST(test_cogging_compensation_subtracts_the_model_from_the_demand);
    RUN_TEST(test_cogging_second_harmonic_carries_its_phase);
    RUN_TEST(test_cogging_compensation_is_clamped_with_the_demand);
    RUN_TEST(test_estimator_torque_input_carries_the_known_cogging_share);
    RUN_TEST(test_foc_limit_holds_the_vector_and_sets_the_integral);
    RUN_TEST(test_speed_target_ramps_at_its_rate_limit);
    RUN_TEST(test_torque_target_steps_at_a_zero_limit);
    RUN_TEST(test_enable_seeds_speed_from_the_estimate_and_torque_from_zero);
    RUN_TEST(test_torque_target_takes_the_last_writer_and_saturates);
    RUN_TEST(test_speed_target_under_the_torque_method_scales_its_limit);
    RUN_TEST(test_torque_request_is_cleared_by_disable_and_fault);
    RUN_TEST(test_ratings_are_the_configured_constants);
    RUN_TEST(test_selected_method_survives_off);
    RUN_TEST(test_init_rejects_off_default_mode);
    RUN_TEST(test_first_enable_precharges_before_alignment);
    RUN_TEST(test_re_enable_precharges_then_drives);
    RUN_TEST(test_cycle_step_follows_the_estimate_every_cycle);
    RUN_TEST(test_cycle_step_wraps_the_offset_angle);
    RUN_TEST(test_cycle_step_not_the_ms_pass_writes_the_outputs);
    RUN_TEST(test_first_enable_aligns_then_captures_offset);
    RUN_TEST(test_subsequent_enable_skips_alignment);

    RUN_TEST(test_init_rejects_zero_pole_pairs);
    RUN_TEST(test_init_rejects_zero_max_velocity);
    RUN_TEST(test_init_rejects_zero_pwm_period);
    RUN_TEST(test_init_rejects_zero_max_voltage);
    RUN_TEST(test_init_rejects_a_current_limit_at_the_phase_trip);
    RUN_TEST(test_init_rejects_a_degenerate_current_loop);
    RUN_TEST(test_init_rejects_a_degenerate_velocity_loop);
    RUN_TEST(test_init_rejects_a_degenerate_position_loop);
    RUN_TEST(test_profile_trapezoid_to_two_pi);
    RUN_TEST(test_profile_triangle_to_half_a_radian);
    RUN_TEST(test_profile_steps_at_a_zero_acceleration_limit);
    RUN_TEST(test_profile_zero_velocity_limit_is_the_maximum_velocity);
    RUN_TEST(test_profile_arrives_at_the_velocity_at_the_point_and_chains);
    RUN_TEST(test_profile_holds_the_velocity_past_the_point);
    RUN_TEST(test_profile_reverses_at_the_acceleration_limit);
    RUN_TEST(test_speed_target_jogs_the_position_target);
    RUN_TEST(test_profile_stops_at_the_travel_bound);
    RUN_TEST(test_setpoint_written_in_precharge_runs_from_the_first_step);
    RUN_TEST(test_position_setpoint_takes_the_last_write);
    RUN_TEST(test_position_setpoint_is_dropped_by_a_disable);
    RUN_TEST(test_position_setpoint_written_while_dark_is_held_to_the_enable);
    RUN_TEST(test_rotor_position_follows_the_angle_across_the_seam);
    RUN_TEST(test_rotor_position_counts_turns);
    RUN_TEST(test_position_first_step_is_the_proportional_velocity_command);
    RUN_TEST(test_position_clamps_the_velocity_command_and_holds_the_integral);
    RUN_TEST(test_position_feeds_the_profile_velocity_forward);
    RUN_TEST(test_init_rejects_a_degenerate_cogging_model);

    return UNITY_END();
}
