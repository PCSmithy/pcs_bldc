// app_motorControl: gating, faults, method selection, precharge, alignment, the
// per-cycle angle, and the six-step and sinusoidal methods.
#include "harness.h"

/* ---- fw~mc_006: enable gating ---- */

// [test->fw~mc_006~1]
static void test_drives_bridge_when_operational_and_unfaulted(void)
{
    TEST_ASSERT_FALSE(bridgeEnabled());
    driveToRunning(10.0f);
    TEST_ASSERT_TRUE(bridgeEnabled());
}

// [test->fw~mc_006~1]
static void test_gate_blocks_bridge_when_not_operational(void)
{
    mock_app_rotorEstimation_setAngle(0.0f);
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_SIX_STEP_TRAP);
    app_motorControl_setVelocity(MOTOR_MC, 10.0f, 0.0f);
    for (int i = 0; i < 5; i++)
    {
        tick();
        advanceTime_ms(200U);
        TEST_ASSERT_FALSE(bridgeEnabled());
    }

    mock_dev_gateDriver_setOperational(GD_MAIN, true);
    tick();
    TEST_ASSERT_TRUE(bridgeEnabled());
}

// [test->fw~mc_006~1]
// A latched fault holds the bridge dark with the gate driver operational.
static void test_gate_blocks_bridge_while_fault_latched(void)
{
    driveToRunning(10.0f);
    setPhaseCurrentsUV(3.0f, 0.0f);
    tick();
    TEST_ASSERT_FALSE(bridgeEnabled());

    setNominalCurrents();
    tick();
    tick();
    TEST_ASSERT_FALSE(bridgeEnabled());
}

// [test->fw~mc_006~1]
// OFF takes effect on the next pass, the speed target left standing.
static void test_off_request_disables_bridge_same_cycle(void)
{
    driveToRunning(10.0f);
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_OFF);
    tick();
    TEST_ASSERT_FALSE(bridgeEnabled());
}

// [test->fw~mc_006~1]
// A zero speed target holds the phases at zero duty with MOE up, cycle after
// cycle; a fault still drops it.
static void test_zero_demand_holds_bridge_enabled(void)
{
    alignAtZero();
    for (int i = 0; i < 9; i++)
    {
        commutateAt(30.0f, 0.0f);
        TEST_ASSERT_TRUE(bridgeEnabled());
        TEST_ASSERT_EQUAL_UINT32(0U, drivenCompare());
    }

    setPhaseCurrentsUV(3.0f, 0.0f);
    tick();
    TEST_ASSERT_FALSE(bridgeEnabled());
}

/* ---- fw~safety_001..003: the fault latches ---- */

// [test->fw~safety_001~1]
static void test_phase_overcurrent_trips_and_latches(void)
{
    driveToRunning(10.0f);
    setPhaseCurrentsUV(3.0f, 0.0f);
    tick();
    TEST_ASSERT_FALSE(bridgeEnabled());

    setNominalCurrents();
    tick();
    TEST_ASSERT_FALSE(bridgeEnabled());
}

// [test->fw~safety_001~1]
static void test_bus_overcurrent_trips(void)
{
    driveToRunning(10.0f);
    setBusCurrent(2.0f);
    tick();
    TEST_ASSERT_FALSE(bridgeEnabled());
}

// [test->fw~safety_001~1]
static void test_current_below_threshold_does_not_trip(void)
{
    driveToRunning(10.0f);
    setPhaseCurrentsUV(1.1f, 0.0f);   // U and the derived W just under 1.2 A
    setBusCurrent(1.4f);
    tick();
    TEST_ASSERT_TRUE(bridgeEnabled());
}

// [test->fw~safety_001~1]
// U + V = 1.5 A puts the derived W at -1.5 A, neither sampled leg over the trip.
static void test_negative_phase_overcurrent_trips(void)
{
    driveToRunning(10.0f);
    setPhaseCurrentsUV(0.75f, 0.75f);
    tick();
    TEST_ASSERT_FALSE(bridgeEnabled());
}

// [test->fw~safety_001~1]
// The clear releases the latch and leaves the bridge dark; a new enable drives.
static void test_clear_fault_releases_latch_and_leaves_the_bridge_dark(void)
{
    driveToRunning(10.0f);
    setPhaseCurrentsUV(3.0f, 0.0f);
    tick();
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_STATE_FAULTED, snapshot().state);

    setNominalCurrents();
    app_motorControl_clearFault(MOTOR_MC);
    tick();
    tick();
    TEST_ASSERT_FALSE(bridgeEnabled());
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_STATE_DISABLED, snapshot().state);
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_MODE_SIX_STEP_TRAP, snapshot().selectedMode);

    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_SIX_STEP_TRAP);
    tick();
    TEST_ASSERT_TRUE(bridgeEnabled());
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_STATE_ENABLED, snapshot().state);
}

// [test->fw~safety_003~1]
// MOE dropped under a driving bridge, as a break does: not asserted again, latched
// by the next pass, driven again only after a clear and an enable. The enable edge
// itself, MOE off before it, latches nothing.
static void test_break_latches_a_fault(void)
{
    driveToRunning(10.0f);
    TEST_ASSERT_TRUE(bridgeEnabled());
    TEST_ASSERT_EQUAL_UINT16(0U, snapshot().faultLatches);

    mock_HW_TIM_assertBreak(BRIDGE_PERIPH);
    for (uint32_t n = 0U; n < 20U; n++)
    {
        cycle();
        TEST_ASSERT_FALSE(bridgeEnabled());   // no cycle writes the enable back
    }
    TEST_ASSERT_EQUAL_UINT16(0U, snapshot().faultLatches);
    app_motorControl_run1ms();
    cycle();
    TEST_ASSERT_FALSE(bridgeEnabled());
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_STATE_FAULTED, snapshot().state);
    TEST_ASSERT_EQUAL_UINT16(1U, snapshot().faultLatches);

    app_motorControl_clearFault(MOTOR_MC);
    tick();
    TEST_ASSERT_FALSE(bridgeEnabled());
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_SIX_STEP_TRAP);
    tick();
    tick();
    TEST_ASSERT_TRUE(bridgeEnabled());
    TEST_ASSERT_EQUAL_UINT16(1U, snapshot().faultLatches);
}

// [test->fw~ops_002~1]
// Each latch counts once: not again while it holds, again after a clear and a new trip.
static void test_fault_latches_are_counted(void)
{
    driveToRunning(10.0f);
    setPhaseCurrentsUV(3.0f, 0.0f);
    tick();
    TEST_ASSERT_EQUAL_UINT16(1U, snapshot().faultLatches);
    tick();
    TEST_ASSERT_EQUAL_UINT16(1U, snapshot().faultLatches);

    setNominalCurrents();
    app_motorControl_clearFault(MOTOR_MC);
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_SIX_STEP_TRAP);
    tick();
    TEST_ASSERT_TRUE(bridgeEnabled());
    TEST_ASSERT_EQUAL_UINT16(1U, snapshot().faultLatches);
    setPhaseCurrentsUV(3.0f, 0.0f);
    tick();
    TEST_ASSERT_EQUAL_UINT16(2U, snapshot().faultLatches);
}

// [test->fw~safety_002~1]
// More than 5 consecutive invalid encoder reads latch a fault; 5 do not.
static void test_encoder_fault_latches_past_limit(void)
{
    driveToRunning(10.0f);
    mock_IO_AS5048_setStatus(ENC_MOTOR, IO_AS5048_STATUS_FAULT);
    for (int i = 0; i < 5; i++) { tick(); }
    TEST_ASSERT_TRUE(bridgeEnabled());

    tick();
    TEST_ASSERT_FALSE(bridgeEnabled());
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_STATE_FAULTED, snapshot().state);
}

// [test->fw~safety_002~1]
static void test_encoder_fault_count_resets_on_valid_read(void)
{
    driveToRunning(10.0f);
    mock_IO_AS5048_setStatus(ENC_MOTOR, IO_AS5048_STATUS_FAULT);
    for (int i = 0; i < 5; i++) { tick(); }
    mock_IO_AS5048_setStatus(ENC_MOTOR, IO_AS5048_STATUS_OK);
    tick();
    mock_IO_AS5048_setStatus(ENC_MOTOR, IO_AS5048_STATUS_FAULT);
    for (int i = 0; i < 5; i++) { tick(); }
    TEST_ASSERT_TRUE(bridgeEnabled());
}

/* ---- the state view (the ring's data, fw~mc_009) ---- */

static void test_state_view_tracks_disabled_enabled_faulted(void)
{
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_STATE_DISABLED, snapshot().state);
    driveToRunning(10.0f);
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_STATE_ENABLED, snapshot().state);
    setPhaseCurrentsUV(3.0f, 0.0f);
    tick();
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_STATE_FAULTED, snapshot().state);
}

static void test_velocity_setpoint_exposed(void)
{
    driveToRunning(10.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.0f, snapshot().velocitySetpoint_radPerSec);
    commutateAt(30.0f, -5.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -5.0f, snapshot().velocitySetpoint_radPerSec);
}

// The measured velocity is the estimator's (fw~est_rotor_002), sign included.
static void test_velocity_view_is_the_estimators(void)
{
    mock_app_rotorEstimation_setVelocity(12.5f);
    tick();
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 12.5f, snapshot().velocityMeasured_radPerSec);
    mock_app_rotorEstimation_setVelocity(-3.25f);
    tick();
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, -3.25f, snapshot().velocityMeasured_radPerSec);
}

/* ---- fw~mc_033: method selection ---- */

// [test->fw~mc_033~1]
// A method request selects it; OFF disables and keeps it. The default is the config's.
static void test_selected_method_survives_off(void)
{
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_MODE_SIX_STEP_TRAP, snapshot().selectedMode);
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_SINUSOIDAL);
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_MODE_SINUSOIDAL, snapshot().selectedMode);
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_OFF);
    tick();
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_MODE_OFF, snapshot().mode);
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_MODE_SINUSOIDAL, snapshot().selectedMode);
}

// [test->fw~mc_033~1]
// Another method requested while six-step drives is ignored; OFF is honoured, and
// the request then selects.
static void test_method_request_while_driving_is_ignored(void)
{
    driveToRunning(10.0f);
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_FOC_TORQUE);
    tick();
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_MODE_SIX_STEP_TRAP, snapshot().selectedMode);
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_MODE_SIX_STEP_TRAP, snapshot().mode);
    TEST_ASSERT_NOT_EQUAL(-1, observedSector());

    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_OFF);
    tick();
    TEST_ASSERT_FALSE(bridgeEnabled());
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_FOC_TORQUE);
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_MODE_FOC_TORQUE, snapshot().selectedMode);
}

/* ---- fw~mc_021: the bootstrap precharge on every enable ---- */

// [test->fw~mc_021~1]
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
    TEST_ASSERT_TRUE(allPhasesAtZeroDuty());

    advanceTime_ms(3U);
    tick();
    TEST_ASSERT_EQUAL_UINT32(DUTY_COMPARE(ALIGN_DUTY_01), phaseCompare(IO_BRIDGE_PHASE_U));
}

// [test->fw~mc_021~1]
// A later enable, already aligned, precharges again and then drives.
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
    TEST_ASSERT_FALSE(allPhasesAtZeroDuty());
    TEST_ASSERT_NOT_EQUAL(DUTY_COMPARE(ALIGN_DUTY_01), phaseCompare(IO_BRIDGE_PHASE_U));
}

/* ---- fw~mc_012: alignment offset capture ---- */

// [test->fw~mc_012~1]
// At 45 deg: the pattern (U at the alignment duty, V and W at zero) for the dwell,
// then the offset; the next cycle's electrical angle reads ~0.
static void test_first_enable_aligns_then_captures_offset(void)
{
    mock_dev_gateDriver_setOperational(GD_MAIN, true);
    mock_app_rotorEstimation_setAngle(DEG2RAD(45.0f));
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_SIX_STEP_TRAP);
    app_motorControl_setVelocity(MOTOR_MC, 10.0f, 0.0f);
    tick();
    advanceTime_ms(PRECHARGE_ADVANCE_MS);

    tick();
    TEST_ASSERT_FALSE(snapshot().isAligned);
    TEST_ASSERT_TRUE(bridgeEnabled());
    TEST_ASSERT_EQUAL_UINT32(DUTY_COMPARE(ALIGN_DUTY_01), phaseCompare(IO_BRIDGE_PHASE_U));
    TEST_ASSERT_EQUAL_UINT32(0U, phaseCompare(IO_BRIDGE_PHASE_V));
    TEST_ASSERT_EQUAL_UINT32(0U, phaseCompare(IO_BRIDGE_PHASE_W));
    TEST_ASSERT_FALSE(phaseFloating(IO_BRIDGE_PHASE_U));
    TEST_ASSERT_FALSE(phaseFloating(IO_BRIDGE_PHASE_V));
    TEST_ASSERT_FALSE(phaseFloating(IO_BRIDGE_PHASE_W));

    advanceTime_ms(200U);
    tick();
    TEST_ASSERT_FALSE(snapshot().isAligned);

    advanceTime_ms(ALIGN_ADVANCE_MS);
    tick();
    TEST_ASSERT_TRUE(snapshot().isAligned);
    tick();
    TEST_ASSERT_FLOAT_WITHIN(0.02f, 0.0f, snapshot().magneticAngle_rad);
}

// [test->fw~mc_012~1]
// A later enable commutates at the drive duty after its precharge: no second dwell.
static void test_subsequent_enable_skips_alignment(void)
{
    driveToRunning(10.0f);
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_OFF);
    tick();
    TEST_ASSERT_FALSE(bridgeEnabled());

    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_SIX_STEP_TRAP);
    tick();
    advanceTime_ms(PRECHARGE_ADVANCE_MS);
    commutateAt(90.0f, 10.0f);
    TEST_ASSERT_TRUE(snapshot().isAligned);
    TEST_ASSERT_EQUAL_UINT32(DUTY_COMPARE(0.5f), drivenCompare());
}

/* ---- fw~mc_019: the per-cycle rotor angle ---- */

// [test->fw~mc_019~1]
// Each cycle's angle is that cycle's estimate, with no 1 ms pass between.
static void test_cycle_step_follows_the_estimate_every_cycle(void)
{
    alignAtZero();
    mock_app_rotorEstimation_setVelocity(21.0f);
    mock_app_rotorEstimation_setAngle(1.0f);
    cycle();
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, snapshot().magneticAngle_rad);
    mock_app_rotorEstimation_setAngle(1.001f);
    cycle();
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.001f, snapshot().magneticAngle_rad);
}

// [test->fw~mc_019~1]
// The offset captured at 2 rad, an estimate of 0.5 rad reads 2 pi - 1.5.
static void test_cycle_step_wraps_the_offset_angle(void)
{
    mock_dev_gateDriver_setOperational(GD_MAIN, true);
    mock_app_rotorEstimation_setAngle(2.0f);
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_SIX_STEP_TRAP);
    tick();
    advanceTime_ms(PRECHARGE_ADVANCE_MS);
    tick();
    advanceTime_ms(ALIGN_ADVANCE_MS);
    tick();

    mock_app_rotorEstimation_setAngle(0.5f);
    cycle();
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, TWO_PI_F - 1.5f, snapshot().magneticAngle_rad);
}

/* ---- fw~mc_015: the step owns the outputs ---- */

// [test->fw~mc_015~1]
// A target the 1 ms pass publishes reaches the bridge only at the next step of the
// channel's own bridge.
static void test_cycle_step_not_the_ms_pass_writes_the_outputs(void)
{
    alignAtZero();
    TEST_ASSERT_EQUAL_UINT32(DUTY_COMPARE(ALIGN_DUTY_01), drivenCompare());

    mock_app_rotorEstimation_setAngle(DEG2RAD(90.0f));
    app_motorControl_setVelocity(MOTOR_MC, 10.0f, 0.0f);
    app_motorControl_run1ms();
    TEST_ASSERT_EQUAL_UINT32(DUTY_COMPARE(ALIGN_DUTY_01), drivenCompare());
    app_motorControl_run50us(IO_BRIDGE_CHANNEL_COUNT);   // no channel's bridge
    TEST_ASSERT_EQUAL_UINT32(DUTY_COMPARE(ALIGN_DUTY_01), drivenCompare());

    cycle();
    TEST_ASSERT_EQUAL_UINT32(DUTY_COMPARE(0.5f), drivenCompare());
    TEST_ASSERT_TRUE(bridgeEnabled());
}

/* ---- fw~mc_011: six-step commutation ---- */

// [test->fw~mc_011~1]
// The rotor angle plus the 90 deg lead lands mid-sector: the duty-d phase at the
// target duty, the duty-0 phase at zero, the third floating.
static void test_sector_table_drives_correct_phases(void)
{
    static const struct {
        float32_t rotorDeg;
        IO_bridge_phase_E dutyD;
        IO_bridge_phase_E duty0;
        IO_bridge_phase_E floatP;
    } cases[] = {
        { 300.0f, IO_BRIDGE_PHASE_U, IO_BRIDGE_PHASE_W, IO_BRIDGE_PHASE_V },
        {   0.0f, IO_BRIDGE_PHASE_V, IO_BRIDGE_PHASE_W, IO_BRIDGE_PHASE_U },
        {  60.0f, IO_BRIDGE_PHASE_V, IO_BRIDGE_PHASE_U, IO_BRIDGE_PHASE_W },
        { 120.0f, IO_BRIDGE_PHASE_W, IO_BRIDGE_PHASE_U, IO_BRIDGE_PHASE_V },
        { 180.0f, IO_BRIDGE_PHASE_W, IO_BRIDGE_PHASE_V, IO_BRIDGE_PHASE_U },
        { 240.0f, IO_BRIDGE_PHASE_U, IO_BRIDGE_PHASE_V, IO_BRIDGE_PHASE_W },
    };

    alignAtZero();
    for (size_t i = 0U; i < COUNTOF(cases); i++)
    {
        commutateAt(cases[i].rotorDeg, 10.0f);
        TEST_ASSERT_TRUE(phaseFloating(cases[i].floatP));
        TEST_ASSERT_FALSE(phaseFloating(cases[i].dutyD));
        TEST_ASSERT_FALSE(phaseFloating(cases[i].duty0));
        TEST_ASSERT_EQUAL_UINT32(DUTY_COMPARE(0.5f), phaseCompare(cases[i].dutyD));
        TEST_ASSERT_EQUAL_UINT32(0U, phaseCompare(cases[i].duty0));
    }
}

// [test->fw~mc_011~1]
// A reversed target flips the field 180 deg: the sector moves 3 of 6.
static void test_reversal_flips_drive_direction(void)
{
    static const float32_t angles[] = { 45.0f, 135.0f, 250.0f };
    alignAtZero();
    for (size_t i = 0U; i < COUNTOF(angles); i++)
    {
        commutateAt(angles[i], 10.0f);
        const int forward = observedSector();
        commutateAt(angles[i], -10.0f);
        const int reverse = observedSector();
        TEST_ASSERT_NOT_EQUAL(-1, forward);
        TEST_ASSERT_NOT_EQUAL(-1, reverse);
        TEST_ASSERT_EQUAL_INT(((forward + 3) % 6), reverse);
    }
}

// [test->fw~mc_011~1]
static void test_duty_proportional_and_clamped(void)
{
    static const struct { float32_t velocity; float32_t duty01; } cases[] = {
        {  10.0f, 0.5f },
        {   5.0f, 0.25f },
        { 100.0f, MAX_DUTY_01 },
    };
    alignAtZero();
    for (size_t i = 0U; i < COUNTOF(cases); i++)
    {
        commutateAt(30.0f, cases[i].velocity);
        TEST_ASSERT_EQUAL_UINT32(DUTY_COMPARE(cases[i].duty01), drivenCompare());
    }
}

/* ---- fw~mc_020: sensored sinusoidal commutation ---- */

// [test->fw~mc_020~1]
// (0, V) at the rotor angle: 90 deg ahead for a positive target, behind for a
// negative one, |V| = |target| / w_max x V_max up to V_max.
static void test_sinusoidal_vector_sits_on_the_q_axis(void)
{
    static const struct { float32_t rotorDeg; float32_t velocity; float32_t leadDeg; float32_t volts; } cases[] = {
        {  30.0f,  10.0f,  90.0f,  5.0f },
        {  30.0f, -10.0f, -90.0f,  5.0f },
        { 200.0f,  20.0f,  90.0f, 10.0f },
        { 200.0f,  30.0f,  90.0f, 10.0f },   // past full scale: setVelocity saturates
        { 300.0f,  -4.0f, -90.0f,  2.0f },
    };
    alignAtZero();
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_SINUSOIDAL);
    for (size_t i = 0U; i < COUNTOF(cases); i++)
    {
        commutateAt(cases[i].rotorDeg, cases[i].velocity);
        const statorVoltage_S v = statorVoltage();
        TEST_ASSERT_FLOAT_WITHIN(0.5f, 0.0f, wrapDeg(v.angleDeg - (cases[i].rotorDeg + cases[i].leadDeg)));
        TEST_ASSERT_FLOAT_WITHIN(0.05f, cases[i].volts, v.magnitude_V);
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
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_STATE_ENABLED, snapshot().state);
}

/* ---- fw~mc_016: the estimator's torque input ---- */

// [test->fw~mc_016~1]
// I_q = -0.3 A (currents (0.3, -0.15, -0.15) at 90 deg off the frame): zero before
// the alignment captures its offset, K_t I_q after it.
static void test_estimator_torque_input_is_zero_before_the_alignment(void)
{
    mock_dev_gateDriver_setOperational(GD_MAIN, true);
    mock_app_rotorEstimation_setAngle(DEG2RAD(90.0f));
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_SIX_STEP_TRAP);
    setPhaseCurrentsUV(0.3f, -0.15f);
    tick();
    advanceTime_ms(PRECHARGE_ADVANCE_MS);
    tick();
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, mock_app_rotorEstimation_torqueInput());

    advanceTime_ms(ALIGN_ADVANCE_MS);
    tick();
    mock_app_rotorEstimation_setAngle(DEG2RAD(180.0f));
    cycle();
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, -0.3f * FOC_KT, mock_app_rotorEstimation_torqueInput());
}

/* ---- init ---- */

// [test->fw~mc_012~1] [test->fw~safety_001~1]
static void test_init_rejects_a_degenerate_drive(void)
{
    freshConfig()->motorPolePairs = 0U;
    TEST_ASSERT_FALSE(reinit());
    freshConfig()->maxVelocity_radPerSec = 0.0f;
    TEST_ASSERT_FALSE(reinit());
    freshConfig()->pwmPeriod_s = 0.0f;
    TEST_ASSERT_FALSE(reinit());
    freshConfig()->maxVoltage_V = 0.0f;
    TEST_ASSERT_FALSE(reinit());
    freshConfig()->defaultMode = APP_MOTORCONTROL_MODE_OFF;
    TEST_ASSERT_FALSE(reinit());
    freshConfig()->alignmentDuty01 = 0.0f;
    TEST_ASSERT_FALSE(reinit());
    freshConfig()->alignmentDuty01 = MAX_DUTY_01 + 0.01f;
    TEST_ASSERT_FALSE(reinit());
    freshConfig()->alignmentDwell_ms = 0U;
    TEST_ASSERT_FALSE(reinit());
    freshConfig()->overcurrentBusTrip_A = 0.0f;
    TEST_ASSERT_FALSE(reinit());
    (void)freshConfig();
    TEST_ASSERT_TRUE(reinit());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_drives_bridge_when_operational_and_unfaulted);
    RUN_TEST(test_gate_blocks_bridge_when_not_operational);
    RUN_TEST(test_gate_blocks_bridge_while_fault_latched);
    RUN_TEST(test_off_request_disables_bridge_same_cycle);
    RUN_TEST(test_zero_demand_holds_bridge_enabled);
    RUN_TEST(test_phase_overcurrent_trips_and_latches);
    RUN_TEST(test_bus_overcurrent_trips);
    RUN_TEST(test_current_below_threshold_does_not_trip);
    RUN_TEST(test_negative_phase_overcurrent_trips);
    RUN_TEST(test_clear_fault_releases_latch_and_leaves_the_bridge_dark);
    RUN_TEST(test_break_latches_a_fault);
    RUN_TEST(test_fault_latches_are_counted);
    RUN_TEST(test_encoder_fault_latches_past_limit);
    RUN_TEST(test_encoder_fault_count_resets_on_valid_read);
    RUN_TEST(test_state_view_tracks_disabled_enabled_faulted);
    RUN_TEST(test_velocity_setpoint_exposed);
    RUN_TEST(test_velocity_view_is_the_estimators);
    RUN_TEST(test_selected_method_survives_off);
    RUN_TEST(test_method_request_while_driving_is_ignored);
    RUN_TEST(test_first_enable_precharges_before_alignment);
    RUN_TEST(test_re_enable_precharges_then_drives);
    RUN_TEST(test_first_enable_aligns_then_captures_offset);
    RUN_TEST(test_subsequent_enable_skips_alignment);
    RUN_TEST(test_cycle_step_follows_the_estimate_every_cycle);
    RUN_TEST(test_cycle_step_wraps_the_offset_angle);
    RUN_TEST(test_cycle_step_not_the_ms_pass_writes_the_outputs);
    RUN_TEST(test_sector_table_drives_correct_phases);
    RUN_TEST(test_reversal_flips_drive_direction);
    RUN_TEST(test_duty_proportional_and_clamped);
    RUN_TEST(test_sinusoidal_vector_sits_on_the_q_axis);
    RUN_TEST(test_sinusoidal_zero_target_centres_the_duties_with_the_bridge_up);
    RUN_TEST(test_estimator_torque_input_is_zero_before_the_alignment);
    RUN_TEST(test_init_rejects_a_degenerate_drive);
    return UNITY_END();
}
