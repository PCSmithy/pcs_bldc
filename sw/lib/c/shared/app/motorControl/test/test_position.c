// app_motorControl: the position method, its setpoint, the move profile and the
// rotor position.
#include "harness.h"

static float32_t positionTarget(void) { return snapshot().positionTarget_rad; }
static float32_t rotorPosition(void) { return snapshot().rotorPosition_rad; }

// The position method at rest: aligned at zero, its first step run with nothing written.
static void driveInPositionMode(void)
{
    alignAtZero();
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_FOC_POSITION);
    tick();
    TEST_ASSERT_TRUE(bridgeEnabled());
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, positionTarget());
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, profileVelocity());
}

// Passes until theta* rests on target, at most maxPasses; the largest theta* through `maxTarget`.
static uint32_t runToRest(float32_t target, uint32_t maxPasses, float32_t * const maxTarget)
{
    uint32_t passes = 0U;
    bool atRest = false;
    float32_t largest = positionTarget();
    while ((!atRest) && (passes < maxPasses))
    {
        tick();
        passes++;
        largest = MAX_OF(largest, positionTarget());
        atRest = ((fabsf(profileVelocity()) < 1e-3f) && (fabsf(positionTarget() - target) < 1e-4f));
    }
    TEST_ASSERT_TRUE_MESSAGE(atRest, "the profile did not come to rest on the target");
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
        reached = (positionTarget() >= (target - 1e-6f));
    }
    TEST_ASSERT_TRUE_MESSAGE(reached, "the target was not reached");
    return passes;
}

// Dark, then the position method re-enabled through its precharge to its first step.
static void disableThenReenable(void)
{
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_OFF);
    tick();
    TEST_ASSERT_FALSE(bridgeEnabled());
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_FOC_POSITION);
    tick();
    advanceTime_ms(PRECHARGE_ADVANCE_MS);
    tick();
    TEST_ASSERT_TRUE(bridgeEnabled());
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

/* ---- fw~mc_031: the move profile ---- */

// [test->fw~mc_031~1]
// 2 pi at 10 rad/s, 50 rad/s^2: at v 200 ms in, at rest on 2 pi at 828 ms, never past.
static void test_profile_trapezoid_to_two_pi(void)
{
    driveInPositionMode();
    app_motorControl_setPositionSetpoint(MOTOR_MC, TWO_PI_F, 10.0f, 50.0f, 0.0f);
    for (uint32_t n = 0U; n < 200U; n++) { tick(); }
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 10.0f, profileVelocity());
    float32_t largest = 0.0f;
    TEST_ASSERT_UINT32_WITHIN(2U, 828U, 200U + runToRest(TWO_PI_F, 2000U, &largest));
    TEST_ASSERT_TRUE(largest <= (TWO_PI_F + 1e-5f));
}

// [test->fw~mc_031~1]
// 0.5 rad under the same limits is a triangle: 5 rad/s at 100 ms, at rest at 200 ms.
static void test_profile_triangle_to_half_a_radian(void)
{
    driveInPositionMode();
    app_motorControl_setPositionSetpoint(MOTOR_MC, 0.5f, 10.0f, 50.0f, 0.0f);
    for (uint32_t n = 0U; n < 100U; n++) { tick(); }
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 5.0f, profileVelocity());
    TEST_ASSERT_UINT32_WITHIN(2U, 200U, 100U + runToRest(0.5f, 1000U, NULL));
}

// [test->fw~mc_031~1]
// A zero acceleration limit steps w_f to v: 2 pi at 10 rad/s in 628 ms.
static void test_profile_steps_at_a_zero_acceleration_limit(void)
{
    driveInPositionMode();
    app_motorControl_setPositionSetpoint(MOTOR_MC, TWO_PI_F, 10.0f, 0.0f, 0.0f);
    tick();
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 10.0f, profileVelocity());
    TEST_ASSERT_UINT32_WITHIN(2U, 628U, 1U + runToRest(TWO_PI_F, 2000U, NULL));
}

// [test->fw~mc_031~1]
// A zero velocity limit stands for w_max: 4 pi at 50 rad/s^2 reaches it before the point.
static void test_profile_zero_velocity_limit_is_the_maximum_velocity(void)
{
    driveInPositionMode();
    app_motorControl_setPositionSetpoint(MOTOR_MC, 2.0f * TWO_PI_F, 0.0f, 50.0f, 0.0f);
    uint32_t passes = 0U;
    while ((profileVelocity() < (0.99f * MAX_VELOCITY)) && (passes < 2000U)) { tick(); passes++; }
    TEST_ASSERT_FLOAT_WITHIN(0.01f * MAX_VELOCITY, MAX_VELOCITY, profileVelocity());
    TEST_ASSERT_TRUE(positionTarget() < (2.0f * TWO_PI_F));
    (void)runToRest(2.0f * TWO_PI_F, 2000U, NULL);
}

// [test->fw~mc_031~1]
// w_s = 10 rad/s: 2 pi reached at 728 ms still at 10 rad/s; 4 pi stopping, written then,
// 728 ms later.
static void test_profile_arrives_at_the_velocity_at_the_point_and_chains(void)
{
    driveInPositionMode();
    app_motorControl_setPositionSetpoint(MOTOR_MC, TWO_PI_F, 10.0f, 50.0f, 10.0f);
    TEST_ASSERT_UINT32_WITHIN(2U, 728U, runToReach(TWO_PI_F, 2000U));
    TEST_ASSERT_FLOAT_WITHIN(1e-2f, 10.0f, profileVelocity());

    app_motorControl_setPositionSetpoint(MOTOR_MC, 2.0f * TWO_PI_F, 10.0f, 50.0f, 0.0f);
    TEST_ASSERT_UINT32_WITHIN(2U, 728U, runToRest(2.0f * TWO_PI_F, 2000U, NULL));
}

// [test->fw~mc_031~1]
// Past the point w_f holds at w_s: a radian on 100 ms later; 4 pi stopping then takes 628 ms.
static void test_profile_holds_the_velocity_past_the_point(void)
{
    driveInPositionMode();
    app_motorControl_setPositionSetpoint(MOTOR_MC, TWO_PI_F, 10.0f, 50.0f, 10.0f);
    (void)runToReach(TWO_PI_F, 2000U);
    const float32_t atPoint = positionTarget();
    for (uint32_t n = 0U; n < 100U; n++)
    {
        tick();
        TEST_ASSERT_FLOAT_WITHIN(1e-2f, 10.0f, profileVelocity());
    }
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.0f, positionTarget() - atPoint);

    app_motorControl_setPositionSetpoint(MOTOR_MC, 2.0f * TWO_PI_F, 10.0f, 50.0f, 0.0f);
    TEST_ASSERT_UINT32_WITHIN(2U, 628U, runToRest(2.0f * TWO_PI_F, 2000U, NULL));
}

// [test->fw~mc_031~1]
// Rewritten to 0 while cruising at 10 rad/s toward 2 pi: w_f falls 0.05 rad/s a pass
// through zero, and theta* comes back to rest on 0.
static void test_profile_reverses_at_the_acceleration_limit(void)
{
    driveInPositionMode();
    app_motorControl_setPositionSetpoint(MOTOR_MC, TWO_PI_F, 10.0f, 50.0f, 0.0f);
    for (uint32_t n = 0U; n < 300U; n++) { tick(); }
    TEST_ASSERT_FLOAT_WITHIN(1e-2f, 10.0f, profileVelocity());

    app_motorControl_setPositionSetpoint(MOTOR_MC, 0.0f, 10.0f, 50.0f, 0.0f);
    float32_t last = profileVelocity();
    bool crossedZero = false;
    for (uint32_t n = 0U; n < 210U; n++)
    {
        tick();
        TEST_ASSERT_FLOAT_WITHIN(1e-3f, -0.05f, profileVelocity() - last);
        last = profileVelocity();
        crossedZero = crossedZero || (last < 0.0f);
    }
    TEST_ASSERT_TRUE(crossedZero);
    (void)runToRest(0.0f, 3000U, NULL);
}

// [test->fw~mc_031~1]
// A speed target under the position method jogs: 2 rad/s moves theta* 0.002 a pass, zero
// holds it, and a setpoint a radian on from that rest is reached in 283 ms.
static void test_speed_target_jogs_the_position_target(void)
{
    driveInPositionMode();
    app_motorControl_setVelocity(MOTOR_MC, 2.0f, 0.0f);
    tick();
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.002f, positionTarget());
    for (uint32_t n = 0U; n < 9U; n++) { tick(); }
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.02f, positionTarget());

    app_motorControl_setVelocity(MOTOR_MC, 0.0f, 0.0f);
    tick();
    const float32_t held = positionTarget();
    for (uint32_t n = 0U; n < 10U; n++) { tick(); }
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, held, positionTarget());

    app_motorControl_setPositionSetpoint(MOTOR_MC, held + 1.0f, 10.0f, 50.0f, 0.0f);
    TEST_ASSERT_UINT32_WITHIN(2U, 283U, runToRest(held + 1.0f, 1000U, NULL));
}

// [test->fw~mc_027~1] [test->fw~mc_031~1]
// A jog at 5 rad/s stopped by a position setpoint, then a speed target of zero at
// 10 rad/s^2: the jog starts from the profile's rest, so theta* holds.
static void test_jog_after_a_position_stop_starts_from_rest(void)
{
    driveInPositionMode();
    app_motorControl_setVelocity(MOTOR_MC, 5.0f, 0.0f);
    for (uint32_t n = 0U; n < 10U; n++) { tick(); }
    const float32_t stop = positionTarget() + 0.3f;
    app_motorControl_setPositionSetpoint(MOTOR_MC, stop, 10.0f, 50.0f, 0.0f);
    (void)runToRest(stop, 1000U, NULL);

    app_motorControl_setVelocity(MOTOR_MC, 0.0f, 10.0f);
    for (uint32_t n = 0U; n < 10U; n++)
    {
        tick();
        TEST_ASSERT_FLOAT_WITHIN(1e-6f, stop, positionTarget());
    }
}

// [test->fw~mc_031~1]
// A jog at w_max stops theta* on theta_max; a jog of -w_max moves it off.
static void test_profile_stops_at_the_travel_bound(void)
{
    driveInPositionMode();
    app_motorControl_setVelocity(MOTOR_MC, MAX_VELOCITY, 0.0f);
    (void)runToReach(POS_MAX, 5000U);
    tick();
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, POS_MAX, positionTarget());
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, profileVelocity());

    app_motorControl_setVelocity(MOTOR_MC, -MAX_VELOCITY, 0.0f);
    tick();
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, -MAX_VELOCITY, profileVelocity());
}

// [test->fw~mc_031~1] [test->fw~mc_030~1]
// 1 rad written during a re-enable's precharge: theta* zero until the first step, at
// rest on 1 rad 283 ms after it.
static void test_setpoint_written_in_precharge_runs_from_the_first_step(void)
{
    driveInPositionMode();
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_OFF);
    tick();
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_FOC_POSITION);
    tick();
    app_motorControl_setPositionSetpoint(MOTOR_MC, 1.0f, 10.0f, 50.0f, 0.0f);
    tick();
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, positionTarget());
    advanceTime_ms(PRECHARGE_ADVANCE_MS);
    tick();
    TEST_ASSERT_UINT32_WITHIN(2U, 283U, 1U + runToRest(1.0f, 1000U, NULL));
}

// [test->fw~mc_031~1]
// 0.09 rad at 20 rad/s stepping: theta* 0.02 a pass, then onto the point with w_f zero.
// A rotor on the line from one pass's theta* to the next's over the pass's 20 cycles, at
// w_f, leaves every cycle's command zero: the loop's target is that line.
static void test_position_target_moves_every_cycle(void)
{
    const float32_t target = 0.09f;
    driveInPositionMode();
    app_motorControl_setPositionSetpoint(MOTOR_MC, target, MAX_VELOCITY, 0.0f, 0.0f);
    float32_t from = 0.0f;
    for (uint32_t pass = 0U; pass < 6U; pass++)
    {
        app_motorControl_run1ms();
        const float32_t to = positionTarget();
        TEST_ASSERT_TRUE(to <= target);
        mock_app_rotorEstimation_setVelocity((fabsf(to - target) < 1e-6f) ? 0.0f : ((to - from) / 1e-3f));
        for (uint32_t n = 1U; n <= 20U; n++)
        {
            const float32_t angle = from + ((to - from) * (float32_t)n / 20.0f);
            mock_app_rotorEstimation_setAngle(angle);
            cycle();
            TEST_ASSERT_FLOAT_WITHIN(0.02f, 0.0f, rotorVoltageAt(RAD2DEG(angle)).vQ);
        }
        from = to;
    }
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, target, from);
}

/* ---- fw~mc_030: the position setpoint ---- */

// [test->fw~mc_030~1]
static void test_position_setpoint_takes_the_last_write(void)
{
    driveInPositionMode();
    app_motorControl_setPositionSetpoint(MOTOR_MC, 1.0f, 10.0f, 50.0f, 0.0f);
    app_motorControl_setPositionSetpoint(MOTOR_MC, 2.0f, 10.0f, 50.0f, 0.0f);
    float32_t largest = 0.0f;
    (void)runToRest(2.0f, 1000U, &largest);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 2.0f, largest);
}

// [test->fw~mc_030~1]
// A setpoint written while driving does not survive a disable.
static void test_position_setpoint_is_dropped_by_a_disable(void)
{
    driveInPositionMode();
    app_motorControl_setPositionSetpoint(MOTOR_MC, 2.0f, 10.0f, 50.0f, 0.0f);
    tick();
    TEST_ASSERT_TRUE(profileVelocity() > 0.0f);
    disableThenReenable();
    for (uint32_t n = 0U; n < 100U; n++) { tick(); }
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, positionTarget());
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, profileVelocity());
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
    TEST_ASSERT_UINT32_WITHIN(2U, 283U, 1U + runToRest(1.0f, 1000U, NULL));
}

// [test->fw~mc_030~1]
// A non-finite acceleration limit steps: theta* comes to rest on the point in 100 passes.
static void test_position_setpoint_with_an_infinite_acceleration_steps(void)
{
    driveInPositionMode();
    app_motorControl_setPositionSetpoint(MOTOR_MC, 1.0f, 10.0f, INFINITY, 0.0f);
    TEST_ASSERT_UINT32_WITHIN(2U, 100U, runToRest(1.0f, 1000U, NULL));
}

/* ---- fw~mc_032: the rotor position ---- */

// [test->fw~mc_032~1]
// The angle stepped across the seam, 6.2 to 0.1 rad and back: +0.1832 rad, then -0.1832.
static void test_rotor_position_follows_the_angle_across_the_seam(void)
{
    driveInPositionMode();
    mock_app_rotorEstimation_setAngle(6.2f);
    tick();
    const float32_t before = rotorPosition();
    mock_app_rotorEstimation_setAngle(0.1f);
    tick();
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.1832f, rotorPosition() - before);
    mock_app_rotorEstimation_setAngle(6.2f);
    tick();
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, before, rotorPosition());
}

// [test->fw~mc_032~1]
// Three turns forward and one back read 4 pi; ten and ten back read zero; zero through a
// disable and the next enable's precharge.
static void test_rotor_position_counts_turns(void)
{
    driveInPositionMode();
    turnRotor(12);
    turnRotor(-4);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 2.0f * TWO_PI_F, rotorPosition());
    turnRotor(40);
    turnRotor(-48);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, rotorPosition());
    turnRotor(5);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.25f * TWO_PI_F, rotorPosition());

    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_OFF);
    tick();
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, rotorPosition());
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_FOC_POSITION);
    tick();
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, rotorPosition());
}

/* ---- fw~mc_029: the position loop ---- */

// [test->fw~mc_029~1]
// The angle stepped by -E after the first step: the next asks i_q* = K_vp K_pp E / K_t.
static void test_position_first_step_is_the_proportional_velocity_command(void)
{
    const float32_t E = 0.2f;   // K_pp E = 6 rad/s, K_vp K_pp E = 0.06 Nm: neither clamps
    const float32_t iQ = FOC_KVP * POS_KPP * E / FOC_KT;
    driveInPositionMode();
    mock_app_rotorEstimation_setAngle(-E);
    cycle();
    const rotorVoltage_S v = rotorVoltageAt(RAD2DEG(-E));
    TEST_ASSERT_FLOAT_WITHIN(0.01f * FOC_KP * iQ, FOC_KP * iQ, v.vQ);
    TEST_ASSERT_FLOAT_WITHIN(0.01f * FOC_KP * iQ, 0.0f, v.vD);
}

// [test->fw~mc_029~1]
// K_pp E at 1.5 w_max: the step asks w_max, and after the angle steps back, zero; the
// second readback carries the first step's current integral, K_i T times its i_q*.
static void test_position_clamps_the_velocity_command_and_holds_the_integral(void)
{
    const float32_t E = 1.5f * MAX_VELOCITY / POS_KPP;
    const float32_t iQmax = FOC_KVP * MAX_VELOCITY / FOC_KT;
    driveInPositionMode();
    mock_app_rotorEstimation_setAngle(-E);
    cycle();
    TEST_ASSERT_FLOAT_WITHIN(0.01f * FOC_KP * iQmax, FOC_KP * iQmax, rotorVoltageAt(RAD2DEG(-E)).vQ);

    mock_app_rotorEstimation_setAngle(0.0f);
    cycle();
    TEST_ASSERT_FLOAT_WITHIN(0.01f * FOC_KP * iQmax, FOC_KI_T * iQmax, rotorVoltageAt(0.0f).vQ);
}

// [test->fw~mc_029~1]
// The angle held, 4 rad at v = F stepping: the first step after the next pass asks
// F (1 + K_pp T), the profile velocity fed forward past the one cycle's error.
static void test_position_feeds_the_profile_velocity_forward(void)
{
    const float32_t F = 10.0f;
    const float32_t iQ = FOC_KVP * F * (1.0f + (POS_KPP * PWM_PERIOD_S)) / FOC_KT;
    driveInPositionMode();
    app_motorControl_setPositionSetpoint(MOTOR_MC, 4.0f, F, 0.0f, 0.0f);
    tick();
    TEST_ASSERT_FLOAT_WITHIN(0.01f * FOC_KP * FOC_KVP * F / FOC_KT, FOC_KP * iQ, rotorVoltageAt(0.0f).vQ);
}

// [test->fw~mc_029~1]
// At V_lim = 0.5 V an error of 0.2 rad asks K_p K_vp K_pp 0.2 / K_t = 0.96 V. 2000 limited
// steps leave S_p and S_v where they were: back on target, the limit lifted, v_q holds.
static void test_position_holds_its_integral_under_the_voltage_limit(void)
{
    setBusVoltage(0.5f * SQRT3_F);
    driveInPositionMode();
    mock_app_rotorEstimation_setAngle(-0.2f);
    for (uint32_t n = 0U; n < 1000U; n++) { tick(); }
    mock_app_rotorEstimation_setAngle(0.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.0f, qVoltageDriftAfterTheLimit(40U));
}

// [test->fw~mc_029~1] K_pp, K_pi at or below zero rejected; [test->fw~mc_031~1] the travel bound likewise
static void test_init_rejects_a_degenerate_position_loop(void)
{
    freshConfig()->foc.positionKp_perSec = 0.0f;
    TEST_ASSERT_FALSE(reinit());
    freshConfig()->foc.positionKi_perSec2 = -1.0f;
    TEST_ASSERT_FALSE(reinit());
    freshConfig()->foc.maxPosition_rad = 0.0f;
    TEST_ASSERT_FALSE(reinit());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_profile_trapezoid_to_two_pi);
    RUN_TEST(test_profile_triangle_to_half_a_radian);
    RUN_TEST(test_profile_steps_at_a_zero_acceleration_limit);
    RUN_TEST(test_profile_zero_velocity_limit_is_the_maximum_velocity);
    RUN_TEST(test_profile_arrives_at_the_velocity_at_the_point_and_chains);
    RUN_TEST(test_profile_holds_the_velocity_past_the_point);
    RUN_TEST(test_profile_reverses_at_the_acceleration_limit);
    RUN_TEST(test_speed_target_jogs_the_position_target);
    RUN_TEST(test_jog_after_a_position_stop_starts_from_rest);
    RUN_TEST(test_profile_stops_at_the_travel_bound);
    RUN_TEST(test_setpoint_written_in_precharge_runs_from_the_first_step);
    RUN_TEST(test_position_target_moves_every_cycle);
    RUN_TEST(test_position_setpoint_takes_the_last_write);
    RUN_TEST(test_position_setpoint_is_dropped_by_a_disable);
    RUN_TEST(test_position_setpoint_written_while_dark_is_held_to_the_enable);
    RUN_TEST(test_position_setpoint_with_an_infinite_acceleration_steps);
    RUN_TEST(test_rotor_position_follows_the_angle_across_the_seam);
    RUN_TEST(test_rotor_position_counts_turns);
    RUN_TEST(test_position_first_step_is_the_proportional_velocity_command);
    RUN_TEST(test_position_clamps_the_velocity_command_and_holds_the_integral);
    RUN_TEST(test_position_feeds_the_profile_velocity_forward);
    RUN_TEST(test_position_holds_its_integral_under_the_voltage_limit);
    RUN_TEST(test_init_rejects_a_degenerate_position_loop);
    return UNITY_END();
}
