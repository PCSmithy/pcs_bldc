// app_motorControl: the field-oriented current, torque and velocity methods, cogging
// compensation, and the speed and torque targets.
#include "harness.h"

// The rotor angles in degrees at which sin(28 theta) = 1, and = -1.
#define COG_PEAK_DEG    (90.0f / 28.0f)
#define COG_TROUGH_DEG  (270.0f / 28.0f)

static void startTorqueAt(float32_t rotorDeg, float32_t velocity)
{
    startMethodAt(APP_MOTORCONTROL_MODE_FOC_TORQUE, rotorDeg, velocity);
}

static void startVelocityAt(float32_t rotorDeg, float32_t velocity)
{
    startMethodAt(APP_MOTORCONTROL_MODE_FOC_VELOCITY, rotorDeg, velocity);
}

// Re-init with the cogging model's orders 28 and 56 at these amplitudes and phases, then align.
static void alignWithCogging(float32_t a1, float32_t phi1, float32_t a2, float32_t phi2, float32_t kcog)
{
    app_motorControl_channelConfig_S * const cfg = freshConfig();
    cfg->foc.cogging[0] = (app_motorControl_cogHarmonic_S){ .order_perRev = 28U, .amplitude_Nm = a1, .phase_rad = phi1 };
    cfg->foc.cogging[1] = (app_motorControl_cogHarmonic_S){ .order_perRev = 56U, .amplitude_Nm = a2, .phase_rad = phi2 };
    cfg->foc.coggingKff = kcog;
    TEST_ASSERT_TRUE(reinit());
    alignAtZero();
}

static void alignWithFeedForward(float32_t kff)
{
    freshConfig()->foc.velocityKff = kff;
    TEST_ASSERT_TRUE(reinit());
    alignAtZero();
}

// The torque method driving at rotor angle 0, both targets at zero.
static void driveInTorqueMode(void)
{
    alignAtZero();
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_FOC_TORQUE);
    app_motorControl_setVelocity(MOTOR_MC, 0.0f, 0.0f);
    tick();
}

static float32_t torqueTarget(void) { return snapshot().torqueTarget_Nm; }
static float32_t speedTarget(void) { return snapshot().velocitySetpoint_radPerSec; }

/* ---- fw~mc_022..024: the current loop ---- */

// [test->fw~mc_022~1]
// Currents at zero: the first step is (0, K_p i_q*), i_q* the signed speed share of
// I_max, every phase enabled.
static void test_foc_first_step_is_kp_times_the_current_target(void)
{
    static const struct { float32_t rotorDeg; float32_t velocity; float32_t iQ; } cases[] = {
        {  30.0f,  10.0f,  0.5f },
        {  30.0f, -10.0f, -0.5f },
        { 200.0f,  20.0f,  1.0f },
        { 200.0f,  30.0f,  1.0f },   // past full scale: setVelocity saturates
        { 120.0f, -20.0f, -1.0f },
        { 300.0f,  -4.0f, -0.2f },
        {  45.0f,   0.0f,  0.0f },
    };
    alignAtZero();
    for (size_t i = 0U; i < COUNTOF(cases); i++)
    {
        startTorqueAt(cases[i].rotorDeg, cases[i].velocity);
        const rotorVoltage_S v = rotorVoltageAt(cases[i].rotorDeg);
        TEST_ASSERT_FLOAT_WITHIN(0.01f * FOC_KP * FOC_I_MAX, FOC_KP * cases[i].iQ, v.vQ);
        TEST_ASSERT_FLOAT_WITHIN(0.01f * FOC_KP * FOC_I_MAX, 0.0f, v.vD);
        TEST_ASSERT_TRUE(bridgeEnabled());
        TEST_ASSERT_FALSE(phaseFloating(IO_BRIDGE_PHASE_U));
        TEST_ASSERT_FALSE(phaseFloating(IO_BRIDGE_PHASE_V));
        TEST_ASSERT_FALSE(phaseFloating(IO_BRIDGE_PHASE_W));
    }
}

// [test->fw~mc_023~1]
// i_q* = I at zero current: K_p I, then K_i T I more a step for 20 steps, v_d zero.
// A gate drop and re-enable, no new selection, starts over.
static void test_foc_integral_ramps_and_restarts_at_an_enable(void)
{
    const float32_t I = 0.5f;   // K_p I + 20 K_i T I = 8 V, under V_lim = 10 V
    const float32_t tolerance = 0.01f * FOC_KP * I;

    alignAtZero();
    startTorqueAt(0.0f, I * MAX_VELOCITY / FOC_I_MAX);
    TEST_ASSERT_FLOAT_WITHIN(tolerance, FOC_KP * I, rotorVoltageAt(0.0f).vQ);
    for (uint32_t n = 1U; n <= 20U; n++)
    {
        cycle();
        const rotorVoltage_S v = rotorVoltageAt(0.0f);
        TEST_ASSERT_FLOAT_WITHIN(tolerance, (FOC_KP * I) + ((float32_t)n * FOC_KI_T * I), v.vQ);
        TEST_ASSERT_FLOAT_WITHIN(tolerance, 0.0f, v.vD);
    }

    mock_dev_gateDriver_setOperational(GD_MAIN, false);
    tick();
    TEST_ASSERT_FALSE(bridgeEnabled());
    mock_dev_gateDriver_setOperational(GD_MAIN, true);
    app_motorControl_setVelocity(MOTOR_MC, I * MAX_VELOCITY / FOC_I_MAX, 0.0f);   // dark dropped the torque request
    tick();
    advanceTime_ms(PRECHARGE_ADVANCE_MS);
    tick();
    TEST_ASSERT_FLOAT_WITHIN(tolerance, FOC_KP * I, rotorVoltageAt(0.0f).vQ);
}

// [test->fw~mc_024~1]
// K_p I between V_lim and 2 V_lim: every step is V_lim on q, and the step after i_q*
// returns to zero is V_lim - K_p I. A d-axis current turns the vector; the limit keeps its angle.
static void test_foc_limit_holds_the_vector_and_sets_the_integral(void)
{
    const float32_t vLim = 4.0f;
    const float32_t I = 0.75f;   // K_p I = 6 V
    const float32_t tolerance = 0.01f * vLim;

    freshConfig()->maxVoltage_V = vLim;
    TEST_ASSERT_TRUE(reinit());
    alignAtZero();
    startTorqueAt(0.0f, I * MAX_VELOCITY / FOC_I_MAX);
    for (uint32_t n = 0U; n < 20U; n++)
    {
        const rotorVoltage_S v = rotorVoltageAt(0.0f);
        TEST_ASSERT_FLOAT_WITHIN(tolerance, vLim, v.vQ);
        TEST_ASSERT_FLOAT_WITHIN(tolerance, 0.0f, v.vD);
        cycle();
    }

    startTorqueAt(0.0f, I * MAX_VELOCITY / FOC_I_MAX);
    app_motorControl_setVelocity(MOTOR_MC, 0.0f, 0.0f);
    app_motorControl_run1ms();
    cycle();
    TEST_ASSERT_FLOAT_WITHIN(tolerance, vLim - (FOC_KP * I), rotorVoltageAt(0.0f).vQ);

    // I_d = 0.3 A at angle 0: the unlimited (-2.4, 6) V scales onto V_lim at v_d / v_q = -0.4
    setPhaseCurrentsUV(0.3f, -0.15f);
    startTorqueAt(0.0f, I * MAX_VELOCITY / FOC_I_MAX);
    const rotorVoltage_S v = rotorVoltageAt(0.0f);
    TEST_ASSERT_FLOAT_WITHIN(tolerance, vLim, sqrtf((v.vD * v.vD) + (v.vQ * v.vQ)));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -0.4f, v.vD / v.vQ);
}

/* ---- fw~mc_025: the velocity method ---- */

// [test->fw~mc_025~1]
// Estimate and target zero, T_d at D: the first step asks i_q* = -k_ff D / K_t.
static void test_foc_velocity_feeds_the_disturbance_estimate_forward(void)
{
    const float32_t D = 0.2f;
    const float32_t iQ = -0.5f * D / FOC_KT;
    alignWithFeedForward(0.5f);
    mock_app_rotorEstimation_setUnmodeledTorque(D);
    startVelocityAt(0.0f, 0.0f);
    const rotorVoltage_S v = rotorVoltageAt(0.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f * FOC_KP * fabsf(iQ), FOC_KP * iQ, v.vQ);
    TEST_ASSERT_FLOAT_WITHIN(0.01f * FOC_KP * fabsf(iQ), 0.0f, v.vD);
}

// [test->fw~mc_025~1]
// k_ff = 1, T_d = -2 K_t I_max: the step clamps to +I_max and S_v is solved net of the
// feed-forward, so with T_d gone the steps ask -I_max (the tick's two steps cancel the
// current loop's integral: the readback is K_p (-I_max)).
static void test_foc_velocity_back_calculates_the_integral_net_of_the_feed_forward(void)
{
    alignWithFeedForward(1.0f);
    mock_app_rotorEstimation_setUnmodeledTorque(-2.0f * FOC_KT * FOC_I_MAX);
    startVelocityAt(0.0f, 0.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f * FOC_KP * FOC_I_MAX, FOC_KP * FOC_I_MAX, rotorVoltageAt(0.0f).vQ);

    mock_app_rotorEstimation_setUnmodeledTorque(0.0f);
    commutateAt(0.0f, 0.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f * FOC_KP * FOC_I_MAX, -FOC_KP * FOC_I_MAX, rotorVoltageAt(0.0f).vQ);
}

// [test->fw~mc_025~1]
// At V_lim = 1 V the first step on 10 rad/s asks K_p K_vp 10 / K_t = 1.6 V. 2000 limited
// steps leave S_v where it was: with the error gone and the limit lifted, v_q holds.
static void test_foc_velocity_holds_its_integral_under_the_voltage_limit(void)
{
    alignAtZero();
    setBusVoltage(1.0f * SQRT3_F);
    startVelocityAt(0.0f, 10.0f);
    for (uint32_t n = 0U; n < 1000U; n++) { tick(); }
    mock_app_rotorEstimation_setVelocity(10.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.0f, qVoltageDriftAfterTheLimit(20U));
}

/* ---- fw~mc_028: cogging compensation ---- */

// [test->fw~mc_028~1]
// Both methods at zero demand: the first step asks -k_cog A sin(28 theta) / K_t on q,
// -0.1 Nm's worth at the peak and nothing at theta = 0.
static void test_cogging_compensation_subtracts_the_model_from_the_demand(void)
{
    static const app_motorControl_mode_E methods[] = { APP_MOTORCONTROL_MODE_FOC_TORQUE, APP_MOTORCONTROL_MODE_FOC_VELOCITY };
    const float32_t comp = 0.5f * 0.2f;   // k_cog A
    const float32_t tolerance = 0.01f * FOC_KP * comp / FOC_KT;
    alignWithCogging(0.2f, 0.0f, 0.0f, 0.0f, 0.5f);
    for (size_t i = 0U; i < COUNTOF(methods); i++)
    {
        startMethodAt(methods[i], COG_PEAK_DEG, 0.0f);
        const rotorVoltage_S peak = rotorVoltageAt(COG_PEAK_DEG);
        TEST_ASSERT_FLOAT_WITHIN(tolerance, -FOC_KP * comp / FOC_KT, peak.vQ);
        TEST_ASSERT_FLOAT_WITHIN(tolerance, 0.0f, peak.vD);
        startMethodAt(methods[i], 0.0f, 0.0f);
        TEST_ASSERT_FLOAT_WITHIN(tolerance, 0.0f, rotorVoltageAt(0.0f).vQ);
    }
}

// [test->fw~mc_028~1]
// The second harmonic alone at phase pi/2 peaks at theta = 0.
static void test_cogging_second_harmonic_carries_its_phase(void)
{
    alignWithCogging(0.0f, 0.0f, 0.2f, 3.14159265f / 2.0f, 1.0f);
    startTorqueAt(0.0f, 0.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f * FOC_KP * 0.2f / FOC_KT, -FOC_KP * 0.2f / FOC_KT, rotorVoltageAt(0.0f).vQ);
}

// [test->fw~mc_028~1]
// A full-scale torque target with the model at its trough: the sum clamps to K_t I_max.
static void test_cogging_compensation_is_clamped_with_the_demand(void)
{
    alignWithCogging(0.2f, 0.0f, 0.0f, 0.0f, 1.0f);
    startTorqueAt(COG_TROUGH_DEG, MAX_VELOCITY);   // full-scale speed is K_t I_max under the torque method
    TEST_ASSERT_FLOAT_WITHIN(0.01f * FOC_KP * FOC_I_MAX, FOC_KP * FOC_I_MAX, rotorVoltageAt(COG_TROUGH_DEG).vQ);
}

// [test->fw~mc_016~1]
// The estimator's torque input carries the compensated cogging share under a
// field-oriented method, K_t I_q alone under six-step.
static void test_estimator_torque_input_carries_the_known_cogging_share(void)
{
    alignWithCogging(0.2f, 0.0f, 0.0f, 0.0f, 0.5f);
    commutateAt(COG_PEAK_DEG, 10.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, mock_app_rotorEstimation_torqueInput());
    startTorqueAt(COG_PEAK_DEG, 0.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.1f, mock_app_rotorEstimation_torqueInput());
}

/* ---- fw~mc_026/027: the targets in effect ---- */

// [test->fw~mc_027~1]
// 10 rad/s at 20 rad/s^2 from zero: 0.02 rad/s a pass, 5 at pass 250, 10 at 500, held.
static void test_speed_target_ramps_at_its_rate_limit(void)
{
    driveToRunning(0.0f);
    app_motorControl_setVelocity(MOTOR_MC, 10.0f, 20.0f);
    float32_t last = speedTarget();
    for (uint32_t pass = 1U; pass <= 500U; pass++)
    {
        tick();
        TEST_ASSERT_FLOAT_WITHIN(0.0002f, 0.02f, speedTarget() - last);
        last = speedTarget();
        if (pass == 250U) { TEST_ASSERT_FLOAT_WITHIN(0.05f, 5.0f, last); }
    }
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 10.0f, speedTarget());
    tick();
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 10.0f, speedTarget());
}

// [test->fw~mc_027~1]
// A zero limit steps the torque target on the next pass; the step asks K_p T / K_t.
static void test_torque_target_steps_at_a_zero_limit(void)
{
    driveInTorqueMode();
    app_motorControl_setTorque(MOTOR_MC, 0.3f, 0.0f);
    tick();
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.3f, torqueTarget());
    const rotorVoltage_S v = rotorVoltageAt(0.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f * FOC_KP * FOC_I_MAX, FOC_KP * 0.3f / FOC_KT, v.vQ);
    TEST_ASSERT_FLOAT_WITHIN(0.01f * FOC_KP * FOC_I_MAX, 0.0f, v.vD);
}

// [test->fw~mc_027~1]
// An enable with the rotor at 5 rad/s: the speed target is 5 on the enable pass and the
// torque target zero, each then moving at its limit.
static void test_enable_seeds_speed_from_the_estimate_and_torque_from_zero(void)
{
    mock_dev_gateDriver_setOperational(GD_MAIN, true);
    mock_app_rotorEstimation_setVelocity(5.0f);
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_FOC_TORQUE);
    app_motorControl_setVelocity(MOTOR_MC, 10.0f, 20.0f);   // and 0.25 Nm at 0.5 Nm/s
    tick();
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 5.0f, speedTarget());
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, torqueTarget());
    tick();
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 5.02f, speedTarget());
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0005f, torqueTarget());
}

// [test->fw~mc_026~1]
// The last writer stands: a torque setpoint, or a speed target under the torque method
// as its share of K_t I_max; saturated, a non-finite one zero; another method's speed
// target leaves it.
static void test_torque_target_takes_the_last_writer_and_saturates(void)
{
    static const struct { float32_t written; float32_t held; } cases[] = {
        {  2.0f * FOC_KT * FOC_I_MAX,  FOC_KT * FOC_I_MAX },
        { -2.0f * FOC_KT * FOC_I_MAX, -FOC_KT * FOC_I_MAX },
        { NAN,                         0.0f },
    };
    driveInTorqueMode();
    app_motorControl_setTorque(MOTOR_MC, 0.4f, 0.0f);
    app_motorControl_setVelocity(MOTOR_MC, MAX_VELOCITY / 2.0f, 0.0f);
    tick();
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, FOC_KT * FOC_I_MAX / 2.0f, torqueTarget());

    app_motorControl_setVelocity(MOTOR_MC, MAX_VELOCITY / 2.0f, 0.0f);
    app_motorControl_setTorque(MOTOR_MC, 0.4f, 0.0f);
    tick();
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.4f, torqueTarget());

    for (size_t i = 0U; i < COUNTOF(cases); i++)
    {
        app_motorControl_setTorque(MOTOR_MC, cases[i].written, 0.0f);
        tick();
        TEST_ASSERT_FLOAT_WITHIN(1e-5f, cases[i].held, torqueTarget());
    }

    startMethodAt(APP_MOTORCONTROL_MODE_SINUSOIDAL, 0.0f, 0.0f);
    app_motorControl_setTorque(MOTOR_MC, -FOC_KT * FOC_I_MAX, 0.0f);
    tick();
    app_motorControl_setVelocity(MOTOR_MC, MAX_VELOCITY, 0.0f);
    tick();
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, -FOC_KT * FOC_I_MAX, torqueTarget());
}

// [test->fw~mc_026~1]
// w_max / 2 at w_max / 2 rad/s^2 under the torque method carries K_t I_max / 2 Nm/s:
// K_t I_max / 4 at 500 ms, K_t I_max / 2 at 1 s.
static void test_speed_target_under_the_torque_method_scales_its_limit(void)
{
    driveInTorqueMode();
    app_motorControl_setVelocity(MOTOR_MC, MAX_VELOCITY / 2.0f, MAX_VELOCITY / 2.0f);
    for (uint32_t n = 0U; n < 500U; n++) { tick(); }
    TEST_ASSERT_FLOAT_WITHIN(0.01f * FOC_KT * FOC_I_MAX, FOC_KT * FOC_I_MAX / 4.0f, torqueTarget());
    for (uint32_t n = 0U; n < 500U; n++) { tick(); }
    TEST_ASSERT_FLOAT_WITHIN(0.01f * FOC_KT * FOC_I_MAX, FOC_KT * FOC_I_MAX / 2.0f, torqueTarget());
}

// [test->fw~mc_026~1]
// A written torque does not survive a disable or a latched fault.
static void test_torque_request_is_cleared_by_disable_and_fault(void)
{
    driveInTorqueMode();
    app_motorControl_setTorque(MOTOR_MC, 0.4f, 0.0f);
    tick();
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.4f, torqueTarget());

    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_OFF);
    tick();
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_FOC_TORQUE);
    tick();
    advanceTime_ms(PRECHARGE_ADVANCE_MS);
    tick();
    tick();
    TEST_ASSERT_TRUE(bridgeEnabled());
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, torqueTarget());

    app_motorControl_setTorque(MOTOR_MC, 0.4f, 0.0f);
    tick();
    setPhaseCurrentsUV(3.0f, 0.0f);
    tick();
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_STATE_FAULTED, snapshot().state);
    setNominalCurrents();
    app_motorControl_clearFault(MOTOR_MC);
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_FOC_TORQUE);
    tick();
    advanceTime_ms(PRECHARGE_ADVANCE_MS);
    tick();
    tick();
    TEST_ASSERT_EQUAL(APP_MOTORCONTROL_STATE_ENABLED, snapshot().state);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, torqueTarget());
}

// [test->fw~mc_026~1]
// A torque written while the bridge is dark stands at the following enable.
static void test_torque_written_while_dark_survives_to_the_enable(void)
{
    driveInTorqueMode();
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_OFF);
    tick();
    tick();
    app_motorControl_setTorque(MOTOR_MC, 0.3f, 0.0f);
    tick();
    app_motorControl_setMode(MOTOR_MC, APP_MOTORCONTROL_MODE_FOC_TORQUE);
    tick();
    advanceTime_ms(PRECHARGE_ADVANCE_MS);
    tick();
    TEST_ASSERT_TRUE(bridgeEnabled());
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.3f, torqueTarget());
}

// [test->fw~conn_server_008~1]
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

/* ---- init ---- */

// [test->fw~mc_022~1]
static void test_init_rejects_a_current_limit_at_the_phase_trip(void)
{
    freshConfig()->foc.maxCurrent_A = PHASE_TRIP_A;
    TEST_ASSERT_FALSE(reinit());
    freshConfig()->foc.maxCurrent_A = 0.0f;
    TEST_ASSERT_FALSE(reinit());
}

// [test->fw~mc_023~1]
// R, L or w_bw zero rejected; w_bw T accepted at 0.19 and rejected at 0.21.
static void test_init_rejects_a_degenerate_current_loop(void)
{
    freshConfig()->foc.motorR_ohm = 0.0f;
    TEST_ASSERT_FALSE(reinit());
    freshConfig()->foc.motorL_H = 0.0f;
    TEST_ASSERT_FALSE(reinit());
    freshConfig()->foc.currentLoopBandwidth_radPerSec = 0.0f;
    TEST_ASSERT_FALSE(reinit());
    freshConfig()->foc.currentLoopBandwidth_radPerSec = 0.19f / PWM_PERIOD_S;
    TEST_ASSERT_TRUE(reinit());
    freshConfig()->foc.currentLoopBandwidth_radPerSec = 0.21f / PWM_PERIOD_S;
    TEST_ASSERT_FALSE(reinit());
}

// [test->fw~mc_025~1]
static void test_init_rejects_a_degenerate_velocity_loop(void)
{
    static const float32_t rejectedKff[] = { -0.01f, 1.01f, NAN };
    static const float32_t acceptedKff[] = { 0.0f, 1.0f };
    freshConfig()->foc.torqueConstant_NmPerA = 0.0f;
    TEST_ASSERT_FALSE(reinit());
    freshConfig()->foc.velocityKp_NmPerRadPerSec = 0.0f;
    TEST_ASSERT_FALSE(reinit());
    freshConfig()->foc.velocityKi_NmPerRad = -1.0f;
    TEST_ASSERT_FALSE(reinit());
    for (size_t i = 0U; i < COUNTOF(rejectedKff); i++)
    {
        freshConfig()->foc.velocityKff = rejectedKff[i];
        TEST_ASSERT_FALSE(reinit());
    }
    for (size_t i = 0U; i < COUNTOF(acceptedKff); i++)
    {
        freshConfig()->foc.velocityKff = acceptedKff[i];
        TEST_ASSERT_TRUE(reinit());
    }
}

// [test->fw~mc_028~1]
static void test_init_rejects_a_degenerate_cogging_model(void)
{
    static const float32_t rejectedKcog[] = { -0.01f, 1.01f, NAN };
    for (size_t i = 0U; i < COUNTOF(rejectedKcog); i++)
    {
        freshConfig()->foc.coggingKff = rejectedKcog[i];
        TEST_ASSERT_FALSE(reinit());
    }
    freshConfig()->foc.cogging[1].amplitude_Nm = -1.0e-3f;
    TEST_ASSERT_FALSE(reinit());
    app_motorControl_channelConfig_S * const cfg = freshConfig();
    cfg->foc.coggingKff = 1.0f;
    cfg->foc.cogging[0].amplitude_Nm = 0.5f;
    TEST_ASSERT_TRUE(reinit());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_foc_first_step_is_kp_times_the_current_target);
    RUN_TEST(test_foc_integral_ramps_and_restarts_at_an_enable);
    RUN_TEST(test_foc_limit_holds_the_vector_and_sets_the_integral);
    RUN_TEST(test_foc_velocity_feeds_the_disturbance_estimate_forward);
    RUN_TEST(test_foc_velocity_back_calculates_the_integral_net_of_the_feed_forward);
    RUN_TEST(test_foc_velocity_holds_its_integral_under_the_voltage_limit);
    RUN_TEST(test_cogging_compensation_subtracts_the_model_from_the_demand);
    RUN_TEST(test_cogging_second_harmonic_carries_its_phase);
    RUN_TEST(test_cogging_compensation_is_clamped_with_the_demand);
    RUN_TEST(test_estimator_torque_input_carries_the_known_cogging_share);
    RUN_TEST(test_speed_target_ramps_at_its_rate_limit);
    RUN_TEST(test_torque_target_steps_at_a_zero_limit);
    RUN_TEST(test_enable_seeds_speed_from_the_estimate_and_torque_from_zero);
    RUN_TEST(test_torque_target_takes_the_last_writer_and_saturates);
    RUN_TEST(test_speed_target_under_the_torque_method_scales_its_limit);
    RUN_TEST(test_torque_request_is_cleared_by_disable_and_fault);
    RUN_TEST(test_torque_written_while_dark_survives_to_the_enable);
    RUN_TEST(test_ratings_are_the_configured_constants);
    RUN_TEST(test_init_rejects_a_current_limit_at_the_phase_trip);
    RUN_TEST(test_init_rejects_a_degenerate_current_loop);
    RUN_TEST(test_init_rejects_a_degenerate_velocity_loop);
    RUN_TEST(test_init_rejects_a_degenerate_cogging_model);
    return UNITY_END();
}
