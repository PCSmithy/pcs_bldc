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
//   - Boundary doubles for IO_AS5048 (inject rotor angle) and dev_gateDriver
//     (force operational) — the two inputs whose real decode chains (SPI frames,
//     I2C status bytes) are irrelevant to what these tests assert.

#include "app_motorControl.h"
#include "mock_IO_AS5048.h"
#include "mock_dev_gateDriver.h"
#include "mock_HW_TIM.h"
#include "mock_HW_ADC.h"
#include "lib_timer.h"
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

// Trip thresholds under test (mirror app_motorControl.c).
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

// Alignment dwell (mirrors app_motorControl.c's ALIGNMENT_DWELL_TIMER_MS); the
// advance margin clears any ms-rounding at the dwell boundary.
#define ALIGN_ADVANCE_MS (600U)

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

static IO_bridge_channelConfig_S     bridgeCfg[IO_BRIDGE_CHANNEL_COUNT];
static IO_bridge_config_S            bridgeConfig;
static app_motorControl_channelConfig_S appCfg[APP_MOTORCONTROL_CHANNEL_COUNT];
static app_motorControl_config_S     appConfig;

static void buildConfigs(void)
{
    bridgeCfg[IO_BRIDGE_CHANNEL_MOTOR] = (IO_bridge_channelConfig_S){
        .phaseU = HW_TIM_CHANNEL_PWM_U,
        .phaseV = HW_TIM_CHANNEL_PWM_V,
        .phaseW = HW_TIM_CHANNEL_PWM_W,
        .phaseCurrent = {
            [IO_BRIDGE_PHASE_U] = { phaseSense[0].ch, phaseSense[0].in, PHASE_BIAS_V, PHASE_V_PER_A },
            [IO_BRIDGE_PHASE_V] = { phaseSense[1].ch, phaseSense[1].in, PHASE_BIAS_V, PHASE_V_PER_A },
            [IO_BRIDGE_PHASE_W] = { phaseSense[2].ch, phaseSense[2].in, PHASE_BIAS_V, PHASE_V_PER_A },
        },
        .busCurrent = { BUS_CH, BUS_IN, 0.0f, BUS_V_PER_A } };
    bridgeConfig = (IO_bridge_config_S){ .channels = bridgeCfg, .numChannels = IO_BRIDGE_CHANNEL_COUNT };

    appCfg[APP_MOTORCONTROL_CHANNEL_MAIN] = (app_motorControl_channelConfig_S){
        .gateDriver = GD_MAIN,
        .bridge = MOTOR_BRIDGE,
        .maxVelocity_radPerSec = MAX_VELOCITY,
        .velocityEstimateFilterTau_s = 0.01f,   // 10 ms, matching fw~est_velocity_001's bounds
        .encoder = ENC_MOTOR,
        // One pole pair keeps electrical angle == rotor angle, so a commutation
        // test can place the field in a chosen sector by setting the rotor angle.
        .motorPolePairs = 1U };
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

static void setPhaseCurrent(IO_bridge_phase_E phase, float32_t amps)
{
    mock_HW_ADC_setVolts(phaseSense[phase].ch, phaseSense[phase].in, PHASE_BIAS_V + (amps * PHASE_V_PER_A));
}

static void setBusCurrent(float32_t amps)
{
    mock_HW_ADC_setVolts(BUS_CH, BUS_IN, amps * BUS_V_PER_A);
}

static void setNominalCurrents(void)
{
    setPhaseCurrent(IO_BRIDGE_PHASE_U, 0.0f);
    setPhaseCurrent(IO_BRIDGE_PHASE_V, 0.0f);
    setPhaseCurrent(IO_BRIDGE_PHASE_W, 0.0f);
    setBusCurrent(0.0f);
}

// Bring the channel to steady commutation: operational, angle fixed, SIX_STEP
// requested, alignment dwell advanced, then a cycle to begin driving.
static void driveToRunning(float32_t velocity)
{
    mock_dev_gateDriver_setOperational(GD_MAIN, true);
    mock_IO_AS5048_setAngle(ENC_MOTOR, 0.0f);
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_SIX_STEP_TRAP);
    app_motorControl_setVelocity(MOTOR_MC, velocity);

    app_motorControl_run1ms();          // enters alignment (bridge enabled)
    advanceTime_ms(ALIGN_ADVANCE_MS);
    app_motorControl_run1ms();          // dwell expires, captures offset
    app_motorControl_run1ms();          // commutating
}

// Complete alignment at rotor angle 0 (capturing offset 0), leaving the channel
// aligned and operational but not yet commutated at a test angle.
static void alignAtZero(void)
{
    mock_dev_gateDriver_setOperational(GD_MAIN, true);
    mock_IO_AS5048_setAngle(ENC_MOTOR, 0.0f);
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_SIX_STEP_TRAP);
    app_motorControl_setVelocity(MOTOR_MC, 10.0f);
    app_motorControl_run1ms();          // alignment begins
    advanceTime_ms(ALIGN_ADVANCE_MS);
    app_motorControl_run1ms();          // dwell expires, offset := 0
}

// Set rotor angle (degrees) and signed velocity, then run one commutation cycle.
static void commutateAt(float32_t rotorDeg, float32_t velocity)
{
    mock_IO_AS5048_setAngle(ENC_MOTOR, DEG2RAD(rotorDeg));
    app_motorControl_setVelocity(MOTOR_MC, velocity);
    app_motorControl_run1ms();
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

    // (dutyD, floating) per six-step table row (fw~mc_011).
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

void setUp(void)
{
    mock_HW_TIM_reset(PERIOD);
    mock_HW_ADC_reset();
    mock_IO_AS5048_reset();
    mock_dev_gateDriver_reset();
    buildConfigs();
    (void)IO_bridge_init(&bridgeConfig);
    (void)app_motorControl_init(&appConfig);
    setNominalCurrents();
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

    setPhaseCurrent(IO_BRIDGE_PHASE_U, 3.0f);  // > 2 A
    app_motorControl_run1ms();
    TEST_ASSERT_FALSE(bridgeEnabled());        // disabled within one cycle

    setPhaseCurrent(IO_BRIDGE_PHASE_U, 0.0f);  // fault cleared physically...
    app_motorControl_run1ms();
    TEST_ASSERT_FALSE(bridgeEnabled());        // ...but the latch persists
}

// [test->fw~safety_001~1]
static void test_bus_overcurrent_trips(void)
{
    driveToRunning(10.0f);
    TEST_ASSERT_TRUE(bridgeEnabled());

    setBusCurrent(2.0f);                       // > 1.5 A
    app_motorControl_run1ms();
    TEST_ASSERT_FALSE(bridgeEnabled());
}

// [test->fw~safety_001~1]
static void test_current_below_threshold_does_not_trip(void)
{
    driveToRunning(10.0f);

    setPhaseCurrent(IO_BRIDGE_PHASE_U, 1.9f);  // just under 2 A
    setBusCurrent(1.4f);                       // just under 1.5 A
    app_motorControl_run1ms();
    TEST_ASSERT_TRUE(bridgeEnabled());         // no trip
}

// [test->fw~safety_001~1]
// A negative phase-current magnitude trips too (bipolar shunt).
static void test_negative_phase_overcurrent_trips(void)
{
    driveToRunning(10.0f);

    setPhaseCurrent(IO_BRIDGE_PHASE_W, -2.5f); // |-2.5| > 2 A
    app_motorControl_run1ms();
    TEST_ASSERT_FALSE(bridgeEnabled());
}

/* ---- fw~mc_006: enable gating ---- */

// [test->fw~mc_006~1]
static void test_gate_blocks_bridge_when_not_operational(void)
{
    // Gate driver never made operational; request a full drive.
    mock_IO_AS5048_setAngle(ENC_MOTOR, 0.0f);
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_SIX_STEP_TRAP);
    app_motorControl_setVelocity(MOTOR_MC, 10.0f);

    for (int i = 0; i < 5; i++)
    {
        app_motorControl_run1ms();
        advanceTime_ms(200U);
        TEST_ASSERT_FALSE(bridgeEnabled());    // held disabled every cycle
    }

    // Becoming operational lets the drive proceed.
    mock_dev_gateDriver_setOperational(GD_MAIN, true);
    app_motorControl_run1ms();                 // alignment begins -> enabled
    TEST_ASSERT_TRUE(bridgeEnabled());
}

// [test->fw~mc_006~1]
// A latched fault holds the bridge disabled even while the gate driver is
// operational — the enable request does not take effect.
static void test_gate_blocks_bridge_while_fault_latched(void)
{
    driveToRunning(10.0f);
    setPhaseCurrent(IO_BRIDGE_PHASE_U, 3.0f);
    app_motorControl_run1ms();
    TEST_ASSERT_FALSE(bridgeEnabled());

    setNominalCurrents();                      // currents nominal, still operational
    app_motorControl_run1ms();
    app_motorControl_run1ms();
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
    app_motorControl_run1ms();
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

    setPhaseCurrent(IO_BRIDGE_PHASE_U, 3.0f);  // a fault still kills the bridge
    app_motorControl_run1ms();
    TEST_ASSERT_FALSE(bridgeEnabled());
}

/* ---- state view + fault-clear (data contract for the ring, fw~mc_009) ---- */

// The snapshot state tracks disabled -> enabled -> faulted.
static void test_state_view_tracks_disabled_enabled_faulted(void)
{
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_STATE_DISABLED, motorState());   // idle at init

    driveToRunning(10.0f);
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_STATE_ENABLED, motorState());    // commutating

    setPhaseCurrent(IO_BRIDGE_PHASE_U, 3.0f);
    app_motorControl_run1ms();
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

/* ---- fw~est_velocity_001: encoder-derived velocity estimate ---- */

static float32_t motorVelocity(void)
{
    app_motorControl_snapshot_S s = { 0 };
    (void)app_motorControl_getSnapshot(MOTOR_MC, &s);
    return s.velocityMeasured_radPerSec;
}

// [test->fw~est_velocity_001~1]
// A constant angle rate converges to the matching velocity: 0.05 rad/tick =
// 50 rad/s, run for 10 filter time constants.
static void test_velocity_estimate_converges_to_angle_rate(void)
{
    float32_t angle = 0.0f;
    for (uint32_t i = 0U; i < 100U; i++)
    {
        angle += 0.05f;
        if (angle >= 6.2831853f)
        {
            angle -= 6.2831853f;
        }
        mock_IO_AS5048_setAngle(ENC_MOTOR, angle);
        app_motorControl_run1ms();
    }
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 50.0f, motorVelocity());
}

// [test->fw~est_velocity_001~1]
// Crossing the 0/2pi wrap produces no velocity discontinuity: once converged,
// every tick through repeated wraps stays near the true rate.
static void test_velocity_estimate_smooth_across_wrap(void)
{
    float32_t angle = 0.0f;
    for (uint32_t i = 0U; i < 100U; i++)
    {
        angle += 0.05f;
        if (angle >= 6.2831853f)
        {
            angle -= 6.2831853f;
        }
        mock_IO_AS5048_setAngle(ENC_MOTOR, angle);
        app_motorControl_run1ms();
    }
    for (uint32_t i = 0U; i < 200U; i++)
    {
        angle += 0.05f;
        if (angle >= 6.2831853f)
        {
            angle -= 6.2831853f;
        }
        mock_IO_AS5048_setAngle(ENC_MOTOR, angle);
        app_motorControl_run1ms();
        TEST_ASSERT_FLOAT_WITHIN(5.0f, 50.0f, motorVelocity());
    }
}

// [test->fw~safety_001~1]
// clearFault releases the latch; with currents nominal the drive resumes.
static void test_clear_fault_releases_latch_and_resumes(void)
{
    driveToRunning(10.0f);
    setPhaseCurrent(IO_BRIDGE_PHASE_U, 3.0f);
    app_motorControl_run1ms();
    TEST_ASSERT_FALSE(bridgeEnabled());
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_STATE_FAULTED, motorState());

    setNominalCurrents();
    app_motorControl_clearFault(MOTOR_MC);
    app_motorControl_run1ms();
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
    for (int i = 0; i < 5; i++) { app_motorControl_run1ms(); }
    TEST_ASSERT_TRUE(bridgeEnabled());                 // 5 is not > 5

    app_motorControl_run1ms();                         // the 6th
    TEST_ASSERT_FALSE(bridgeEnabled());
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_STATE_FAULTED, motorState());
}

// [test->fw~safety_002~1]
// A valid read resets the consecutive count, so interrupted glitches never trip.
static void test_encoder_fault_count_resets_on_valid_read(void)
{
    driveToRunning(10.0f);

    mock_IO_AS5048_setStatus(ENC_MOTOR, IO_AS5048_STATUS_FAULT);
    for (int i = 0; i < 5; i++) { app_motorControl_run1ms(); }

    mock_IO_AS5048_setStatus(ENC_MOTOR, IO_AS5048_STATUS_OK);
    app_motorControl_run1ms();                         // resets the count

    mock_IO_AS5048_setStatus(ENC_MOTOR, IO_AS5048_STATUS_FAULT);
    for (int i = 0; i < 5; i++) { app_motorControl_run1ms(); }
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
        { 270.0f, IO_BRIDGE_PHASE_U, IO_BRIDGE_PHASE_V, IO_BRIDGE_PHASE_W },
        { 330.0f, IO_BRIDGE_PHASE_U, IO_BRIDGE_PHASE_W, IO_BRIDGE_PHASE_V },
        {  30.0f, IO_BRIDGE_PHASE_V, IO_BRIDGE_PHASE_W, IO_BRIDGE_PHASE_U },
        {  90.0f, IO_BRIDGE_PHASE_V, IO_BRIDGE_PHASE_U, IO_BRIDGE_PHASE_W },
        { 150.0f, IO_BRIDGE_PHASE_W, IO_BRIDGE_PHASE_U, IO_BRIDGE_PHASE_V },
        { 210.0f, IO_BRIDGE_PHASE_W, IO_BRIDGE_PHASE_V, IO_BRIDGE_PHASE_U },
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

/* ---- fw~mc_012: alignment offset capture ---- */

// [test->fw~mc_012~1]
static void test_first_enable_aligns_then_captures_offset(void)
{
    // Enable at a non-zero rotor angle so the captured offset is observable.
    mock_dev_gateDriver_setOperational(GD_MAIN, true);
    mock_IO_AS5048_setAngle(ENC_MOTOR, DEG2RAD(45.0f));
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_SIX_STEP_TRAP);
    app_motorControl_setVelocity(MOTOR_MC, 10.0f);

    // Dwell cycle 1: alignment pattern (U & V driven, U at align duty, W float),
    // not yet aligned.
    app_motorControl_run1ms();
    app_motorControl_snapshot_S snap = { 0 };
    (void)app_motorControl_getSnapshot(MOTOR_MC, &snap);
    TEST_ASSERT_FALSE(snap.isAligned);
    TEST_ASSERT_TRUE(bridgeEnabled());
    TEST_ASSERT_EQUAL_UINT32(DUTY_COMPARE(ALIGN_DUTY_01), phaseCompare(IO_BRIDGE_PHASE_U));
    TEST_ASSERT_EQUAL_UINT32(0U, phaseCompare(IO_BRIDGE_PHASE_V));
    TEST_ASSERT_FALSE(phaseFloating(IO_BRIDGE_PHASE_U));
    TEST_ASSERT_TRUE(phaseFloating(IO_BRIDGE_PHASE_W));

    // Still aligning partway through the dwell.
    advanceTime_ms(200U);
    app_motorControl_run1ms();
    (void)app_motorControl_getSnapshot(MOTOR_MC, &snap);
    TEST_ASSERT_FALSE(snap.isAligned);

    // After the dwell the offset is captured; the following cycle commutates on
    // it, so the electrical angle (rotor 45deg minus offset 45deg) reads ~0.
    advanceTime_ms(ALIGN_ADVANCE_MS);
    app_motorControl_run1ms();
    (void)app_motorControl_getSnapshot(MOTOR_MC, &snap);
    TEST_ASSERT_TRUE(snap.isAligned);

    app_motorControl_run1ms();
    (void)app_motorControl_getSnapshot(MOTOR_MC, &snap);
    TEST_ASSERT_FLOAT_WITHIN(0.02f, 0.0f, snap.magneticAngle_rad);
}

// [test->fw~mc_012~1]
// A subsequent enable commutates immediately on the stored offset — no second
// alignment dwell.
static void test_subsequent_enable_skips_alignment(void)
{
    alignAtZero();                 // aligned, offset 0
    app_motorControl_run1ms();     // commutating

    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_OFF);
    app_motorControl_run1ms();
    TEST_ASSERT_FALSE(bridgeEnabled());

    // Re-enable: one cycle should commutate at the drive duty (0.5), not fall
    // back to the alignment duty (0.1).
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_SIX_STEP_TRAP);
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
    RUN_TEST(test_velocity_estimate_converges_to_angle_rate);
    RUN_TEST(test_velocity_estimate_smooth_across_wrap);
    RUN_TEST(test_clear_fault_releases_latch_and_resumes);

    RUN_TEST(test_encoder_fault_latches_past_limit);
    RUN_TEST(test_encoder_fault_count_resets_on_valid_read);

    RUN_TEST(test_sector_table_drives_correct_phases);
    RUN_TEST(test_reversal_flips_drive_direction);
    RUN_TEST(test_duty_proportional_and_clamped);

    RUN_TEST(test_first_enable_aligns_then_captures_offset);
    RUN_TEST(test_subsequent_enable_skips_alignment);

    RUN_TEST(test_init_rejects_zero_pole_pairs);
    RUN_TEST(test_init_rejects_zero_max_velocity);

    return UNITY_END();
}
