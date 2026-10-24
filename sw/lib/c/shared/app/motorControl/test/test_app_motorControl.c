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
#define PHASE_TRIP_A    (2.0f)
#define BUS_TRIP_A      (1.5f)

// Drive parameters (mirror the module + the config above).
#define MAX_VELOCITY    (20.0f)         // configured max speed (rad/s)
#define ALIGN_DUTY_01   (0.1f)          // mirrors ALIGNMENT_DUTY_CYCLE
#define MAX_DUTY_01     (0.9f)          // mirrors APP_MOTORCONTROL_MAX_DUTY_01
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
    app_motorControl_setVelocity(MOTOR_MC, velocity);

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
    app_motorControl_setVelocity(MOTOR_MC, 10.0f);
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
    app_motorControl_setVelocity(MOTOR_MC, velocity);
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

    setPhaseCurrentsUV(3.0f, 0.0f);            // U > 2 A
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

    setPhaseCurrentsUV(1.9f, 0.0f);            // U and the derived W just under 2 A
    setBusCurrent(1.4f);                       // just under 1.5 A
    tick();
    TEST_ASSERT_TRUE(bridgeEnabled());         // no trip
}

// [test->fw~safety_001~1]
// A negative phase-current magnitude trips too (bipolar shunt). W is derived, so
// the scenario is expressed through the sampled legs: U + V = 2.5 A puts W at
// -2.5 A while neither sampled leg is over the trip.
static void test_negative_phase_overcurrent_trips(void)
{
    driveToRunning(10.0f);

    setPhaseCurrentsUV(1.25f, 1.25f);          // W = -(1.25 + 1.25), |-2.5| > 2 A
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
    app_motorControl_setVelocity(MOTOR_MC, 10.0f);

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
    app_motorControl_setVelocity(MOTOR_MC, 10.0f);
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
    app_motorControl_setVelocity(MOTOR_MC, 10.0f);

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
    app_motorControl_setVelocity(MOTOR_MC, 10.0f);
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
    app_motorControl_setVelocity(MOTOR_MC, 10.0f);

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

    RUN_TEST(test_encoder_fault_latches_past_limit);
    RUN_TEST(test_encoder_fault_count_resets_on_valid_read);

    RUN_TEST(test_sector_table_drives_correct_phases);
    RUN_TEST(test_reversal_flips_drive_direction);
    RUN_TEST(test_duty_proportional_and_clamped);

    RUN_TEST(test_sinusoidal_vector_sits_on_the_q_axis);
    RUN_TEST(test_sinusoidal_zero_target_centres_the_duties_with_the_bridge_up);
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

    return UNITY_END();
}
